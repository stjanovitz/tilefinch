#ifndef TILEFINCH_STYLE_PAINT_INTERNAL_H
#define TILEFINCH_STYLE_PAINT_INTERNAL_H

#include "tilefinch/style.h"

/* Uncommon retained paint state shared by style resolution and layout. */
#define STYLE_PAINT_LAYER_LIMIT 3u
#define STYLE_PAINT_STACK_LIMIT 255u
#define STYLE_PAINT_STACK_BLOCK_SIZE 4u
#define STYLE_PAINT_STACK_BLOCK_COUNT \
    ((STYLE_PAINT_STACK_LIMIT + STYLE_PAINT_STACK_BLOCK_SIZE - 1u) \
     / STYLE_PAINT_STACK_BLOCK_SIZE)
#define STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE UINT8_C(1)
#define STYLE_PAINT_COMPONENT_BACKGROUND_BOX   UINT8_C(2)
#define STYLE_PAINT_COMPONENT_MASK_IMAGE       UINT8_C(4)
#define STYLE_PAINT_COMPONENT_TABLE_SPACING    UINT8_C(8)
#define STYLE_PAINT_COMPONENT_TRANSFORM_ORIGIN UINT8_C(16)
#define STYLE_PAINT_COMPONENT_TEXT_SHADOW      UINT8_C(32)
#define STYLE_PAINT_COMPONENT_BOX_SHADOW       UINT8_C(64)
#define STYLE_PAINT_POSITION_FROM_RIGHT UINT8_C(1)
#define STYLE_PAINT_POSITION_FROM_BOTTOM UINT8_C(2)
#define STYLE_PAINT_POSITION_X_PIXELS UINT8_C(4)
#define STYLE_PAINT_POSITION_Y_PIXELS UINT8_C(8)

typedef struct StylePaintStack StylePaintStack;

typedef enum {
    STYLE_PAINT_IMAGE_NONE,
    STYLE_PAINT_IMAGE_URL,
    STYLE_PAINT_IMAGE_GRADIENT
} StylePaintImageKind;

typedef enum {
    STYLE_PAINT_BOX_BORDER,
    STYLE_PAINT_BOX_PADDING,
    STYLE_PAINT_BOX_CONTENT
} StylePaintBox;

typedef struct {
    const char *image;
    StyleGradient gradient;
    uint16_t width;
    uint16_t height;
    int16_t position_x;
    int16_t position_y;
    uint8_t kind;
    uint8_t fit;
    uint8_t flags;
    uint8_t origin;
    uint8_t clip;
    uint8_t position_edges;
    uint8_t reserved[2];
} StylePaintLayer;

struct StylePaintStack {
    uint8_t background_count;
    uint8_t mask_count;
    uint8_t table_spacing_x;
    uint8_t table_spacing_y;
    uint8_t components;
    uint8_t text_shadow_count;
    uint8_t box_shadow_count;
    uint8_t reserved;
    uint16_t transform_origin_x;
    uint16_t transform_origin_y;
    StylePaintLayer backgrounds[STYLE_PAINT_LAYER_LIMIT];
    StylePaintLayer masks[STYLE_PAINT_LAYER_LIMIT];
    StyleBoxShadow text_shadows[STYLE_BOX_SHADOW_LIMIT];
    StyleBoxShadow box_shadows[STYLE_BOX_SHADOW_LIMIT];
};

/* Rare compositing state shares the optional paint stack instead of growing
   every ComputedStyle.  The backdrop radius is deliberately capped at three
   pixels: it is a compatibility effect for small fixed/sticky chrome, not an
   unbounded full-page post-processing pass. */
#define STYLE_PAINT_MIX_BLEND_MASK UINT8_C(0x03)
#define STYLE_PAINT_BACKDROP_BLUR_SHIFT 2
#define STYLE_PAINT_BACKDROP_BLUR_MASK UINT8_C(0x0c)
#define STYLE_PAINT_FILTER_LOW_AMOUNT UINT8_C(0x10)

enum {
    STYLE_MIX_BLEND_NORMAL,
    STYLE_MIX_BLEND_MULTIPLY,
    STYLE_MIX_BLEND_SCREEN,
    STYLE_MIX_BLEND_DARKEN
};

struct StylePaintStorage {
    /* Layout can retain a resolved stack pointer while lazy pseudo-style
       resolution interns more paint. Small fixed blocks keep those addresses
       stable without eagerly charging the PSP for all 255 entries. */
    StylePaintStack **blocks;
    size_t count;
    size_t capacity;
};

static inline StylePaintStack *style_paint_storage_slot(
    StylePaintStorage *storage, size_t index)
{
    if (storage == NULL || index >= STYLE_PAINT_STACK_LIMIT) return NULL;
    size_t block_index = index / STYLE_PAINT_STACK_BLOCK_SIZE;
    if (storage->blocks == NULL || block_index >= storage->capacity) {
        return NULL;
    }
    StylePaintStack *block = storage->blocks[block_index];
    return block == NULL
        ? NULL : &block[index % STYLE_PAINT_STACK_BLOCK_SIZE];
}

static inline const StylePaintStack *style_paint_storage_const_slot(
    const StylePaintStorage *storage, size_t index)
{
    return style_paint_storage_slot((StylePaintStorage *) storage, index);
}

const StylePaintStack *stylesheet_paint_stack(
    const Stylesheet *sheet, uint8_t one_based_id);

#endif
