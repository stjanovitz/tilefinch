#include "tilefinch/render.h"
#include "tilefinch/integer_math.h"
#include "tilefinch/pixel_math.h"
#include "tilefinch/platform.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_RENDER, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_RENDER, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_RENDER, (p), (s))

enum { RENDER_GRADIENT_CACHE_ENTRIES = 2 };

static void canvas_overlay_cache_clear(TileCache *cache);

static int render_saturating_add_int(int left, int right)
{
    int64_t sum = (int64_t) left + right;
    return sum > INT_MAX ? INT_MAX : (sum < INT_MIN ? INT_MIN : (int) sum);
}

static int render_saturating_subtract_int(int left, int right)
{
    int64_t difference = (int64_t) left - right;
    return difference > INT_MAX ? INT_MAX
        : (difference < INT_MIN ? INT_MIN : (int) difference);
}

typedef struct {
    size_t slot;
    const DrawCommand *command;
    uint64_t last_used;
    bool valid;
    int32_t step_x;
    int32_t step_y;
    int64_t origin;
    uint32_t lut[256];
} RenderGradientCacheEntry;

struct RenderGradientCache {
    uint64_t clock;
    RenderGradientCacheEntry entries[RENDER_GRADIENT_CACHE_ENTRIES];
};

#include "render/raster_primitives.inc"
#include "render/glyph_raster.inc"
#include "render/visual_raster.inc"

static void rasterize_tile(TileCache *cache, RenderTile *tile)
{
    uint64_t started = render_now_us();
    int left = tile->tile_x * TILEFINCH_TILE_SIZE;
    int top = tile->tile_y * TILEFINCH_TILE_SIZE;
    int right = left + TILEFINCH_TILE_SIZE;
    int bottom = top + TILEFINCH_TILE_SIZE;
    RasterTarget target = tile_raster_target(tile, left, top, right, bottom);
    uint32_t page_background = cache->layout->page_background;
    if (cache->forced_dark) {
        page_background = forced_dark_color(
            page_background, FORCED_DARK_SURFACE);
    }
    uint16_t background = rgb565_surface(page_background);
    for (size_t i = 0; i < TILEFINCH_TILE_SIZE * TILEFINCH_TILE_SIZE; i++) {
        tile->pixels[i] = background;
    }

    if (tile->tile_y >= 0
        && (size_t) tile->tile_y < cache->layout->spatial_band_count
        && cache->layout->spatial_band_offsets != NULL) {
        size_t band = (size_t) tile->tile_y;
        size_t local = cache->layout->spatial_band_offsets[band];
        size_t local_end = cache->layout->spatial_band_offsets[band + 1];
        size_t global = 0;
        while (local < local_end
               || global < cache->layout->spatial_global_count) {
            uint32_t local_order = local < local_end
                ? cache->layout->spatial_band_orders[local] : UINT32_MAX;
            uint32_t global_order = global < cache->layout->spatial_global_count
                ? cache->layout->spatial_global_orders[global] : UINT32_MAX;
            uint32_t order = local_order < global_order
                             ? local_order : global_order;
            if (local_order < global_order) local++;
            else global++;
            size_t index = cache->layout->paint_order[order];
            if (cache->layout->command_flags[index]
                & (LAYOUT_COMMAND_OVERFLOW
                   | LAYOUT_COMMAND_LATE_POSITIONED)) continue;
            cache->command_candidates++;
            rasterize_command(cache, &target,
                              &cache->layout->commands[index]);
        }
    } else {
        for (size_t order = 0; order < cache->layout->count; order++) {
            size_t i = cache->layout->paint_order_count == cache->layout->count
                       ? cache->layout->paint_order[order] : order;
            if (cache->layout->command_flags != NULL
                && cache->layout->command_flags[i] != 0) continue;
            if (cache->layout->command_flags == NULL
                && (command_in_overflow(cache->layout, i))) continue;
            cache->command_candidates++;
            rasterize_command(cache, &target, &cache->layout->commands[i]);
        }
    }
    cache->rasterized++;
    uint64_t elapsed = render_now_us() - started;
    cache->raster_us += elapsed;
    if (elapsed > cache->max_raster_us) cache->max_raster_us = elapsed;
}

static RenderTile *find_tile(TileCache *cache, int tile_x, int tile_y)
{
    for (size_t i = 0; i < cache->tile_capacity; i++) {
        RenderTile *tile = &cache->tiles[i];
        if (tile->valid && tile->tile_x == tile_x && tile->tile_y == tile_y) {
            return tile;
        }
    }
    return NULL;
}

static RenderTile *tile_victim(TileCache *cache)
{
    RenderTile *victim = NULL;
    for (size_t i = 0; i < cache->tile_capacity; i++) {
        RenderTile *tile = &cache->tiles[i];
        if (!tile->valid) return tile;
        if (victim == NULL || tile->last_used < victim->last_used) victim = tile;
    }
    return victim;
}

static bool tile_frame_ordinal(const RenderTile *tile, int first_tx,
                               int first_ty, size_t columns, size_t rows,
                               size_t *ordinal)
{
    if (tile == NULL || !tile->valid || tile->tile_x < first_tx
        || tile->tile_y < first_ty
        || (size_t) (tile->tile_x - first_tx) >= columns
        || (size_t) (tile->tile_y - first_ty) >= rows) return false;
    *ordinal = (size_t) (tile->tile_y - first_ty) * columns
               + (size_t) (tile->tile_x - first_tx);
    return true;
}

static RenderTile *tile_victim_outside_set(TileCache *cache,
                                           uint64_t desired,
                                           int first_tx, int first_ty,
                                           size_t columns, size_t rows)
{
    RenderTile *victim = NULL;
    for (size_t i = 0; i < cache->tile_capacity; i++) {
        RenderTile *tile = &cache->tiles[i];
        if (!tile->valid) return tile;
        size_t ordinal = 0;
        bool wanted = tile_frame_ordinal(tile, first_tx, first_ty, columns,
                                         rows, &ordinal)
                      && (desired & (UINT64_C(1) << ordinal)) != 0;
        if (wanted) continue;
        if (victim == NULL || tile->last_used < victim->last_used) victim = tile;
    }
    return victim != NULL ? victim : tile_victim(cache);
}

static RenderTile *render_tile_miss(TileCache *cache, int tile_x, int tile_y,
                                    RenderTile *victim, bool count_miss)
{
    if (count_miss) cache->misses++;
    if (victim == NULL) victim = tile_victim(cache);
    if (victim == NULL) return NULL;
    if (victim->valid) cache->evictions++;
    victim->valid = true;
    victim->tile_x = tile_x;
    victim->tile_y = tile_y;
    victim->last_used = ++cache->clock;
    rasterize_tile(cache, victim);
    return victim;
}

static RenderTile *ensure_tile(TileCache *cache, int tile_x, int tile_y)
{
    RenderTile *tile = find_tile(cache, tile_x, tile_y);
    if (tile != NULL) {
        cache->hits++;
        tile->last_used = ++cache->clock;
        return tile;
    }
    cache->misses++;
    return render_tile_miss(cache, tile_x, tile_y, NULL, false);
}

static bool tile_cache_prepare_layout(TileCache *cache,
                                      const LayoutDocument *layout)
{
    cache->source_layout = layout;
    if (viewport_context_is_scaled(&layout->viewport)) {
        if (!layout_clone_visual(&cache->visual_layout, layout)) return false;
        cache->layout = &cache->visual_layout;
        cache->owns_visual_layout = true;
    } else {
        cache->layout = layout;
        cache->owns_visual_layout = false;
    }
    return true;
}

static void tile_cache_release_visual_layout(TileCache *cache)
{
    if (cache->owns_visual_layout) layout_destroy(&cache->visual_layout);
    cache->owns_visual_layout = false;
    cache->layout = NULL;
    cache->source_layout = NULL;
}

static void tile_cache_sync_visual_scroll(TileCache *cache)
{
    if (cache == NULL || !cache->owns_visual_layout
        || cache->source_layout == NULL) return;
    size_t count = cache->source_layout->node_box_count;
    if (count > cache->visual_layout.node_box_count) {
        count = cache->visual_layout.node_box_count;
    }
    for (size_t i = 0; i < count; i++) {
        cache->visual_layout.node_boxes[i].scroll_x = viewport_css_to_device(
            &cache->source_layout->viewport,
            cache->source_layout->node_boxes[i].scroll_x);
        cache->visual_layout.node_boxes[i].scroll_y = viewport_css_to_device(
            &cache->source_layout->viewport,
            cache->source_layout->node_boxes[i].scroll_y);
    }
}

bool tile_cache_init(TileCache *cache, Budget *budget,
                     const LayoutDocument *layout, size_t tile_capacity)
{
    if (cache == NULL || budget == NULL || layout == NULL || tile_capacity == 0) {
        return false;
    }
    memset(cache, 0, sizeof(*cache));
    cache->budget = budget;
    if (!tile_cache_prepare_layout(cache, layout)) {
        memset(cache, 0, sizeof(*cache));
        return false;
    }
    cache->tiles = budget_calloc(budget, tile_capacity, sizeof(*cache->tiles));
    if (cache->tiles == NULL) {
        tile_cache_release_visual_layout(cache);
        memset(cache, 0, sizeof(*cache));
        return false;
    }
    cache->tile_capacity = tile_capacity;
    cache->idle_glyph_warming_disabled =
        getenv("TILEFINCH_ENABLE_IDLE_GLYPH_WARM") == NULL;
    cache->glyph_cache = budget_calloc(
        budget, TILEFINCH_GLYPH_CACHE_ENTRIES, sizeof(*cache->glyph_cache));
    if (cache->glyph_cache != NULL) {
        cache->glyph_cache_capacity = TILEFINCH_GLYPH_CACHE_ENTRIES;
    }
    return true;
}

void tile_cache_set_fast_text_raster(TileCache *cache, bool enabled)
{
    if (cache == NULL || cache->frames_rendered != 0
        || cache->glyph_cache_count != 0) {
        return;
    }
    cache->fast_text_raster = enabled;
}

void tile_cache_set_forced_dark(TileCache *cache, bool enabled)
{
    if (cache == NULL || cache->forced_dark == enabled) return;
    cache->forced_dark = enabled;
    tile_cache_invalidate_glyphs(cache);
}

void tile_cache_invalidate_glyphs(TileCache *cache)
{
    if (cache == NULL) return;
    glyph_cache_clear(cache, true);
    tile_cache_repaint_glyphs(cache);
}

void tile_cache_repaint_glyphs(TileCache *cache)
{
    if (cache == NULL) return;
    tile_cache_cancel_frame_work(cache);
    tile_cache_cancel_idle_work(cache);
    for (size_t i = 0; i < cache->tile_capacity; i++) {
        cache->tiles[i].valid = false;
    }
    cache->fixed_ready = false;
    cache->fixed_backdrop = false;
    cache->fixed_backdrop_masked = false;
    canvas_overlay_cache_clear(cache);
    cache->last_frame_scroll_valid = false;
    cache->invalidations++;
}

static void paint_overflow_command(TileCache *cache, uint16_t *frame,
                                   size_t index, int scroll_y,
                                   int viewport_width, int viewport_height)
{
    int dx = 0, dy = 0, clip_left = 0, clip_top = 0;
    int clip_right = 0, clip_bottom = 0;
    const OverflowGeometry *geometry = overflow_cached_geometry(cache, index);
    if (geometry != NULL) {
        if (!geometry->found
            || geometry->clip_right <= geometry->clip_left
            || geometry->clip_bottom <= geometry->clip_top) return;
        dx = geometry->dx;
        dy = geometry->dy;
        clip_left = geometry->clip_left;
        clip_top = geometry->clip_top;
        clip_right = geometry->clip_right;
        clip_bottom = geometry->clip_bottom;
    } else if (!overflow_command_geometry(
                   cache->layout, index, &dx, &dy, &clip_left, &clip_top,
                   &clip_right, &clip_bottom)) {
        return;
    }
    DrawCommand command = cache->layout->commands[index];
    command.x += dx;
    command.y += dy;
    RoundedClip rounded_clips[MAXIMUM_ROUNDED_CLIPS];
    size_t rounded_clip_count = geometry != NULL
        ? overflow_cached_rounded_clips(cache, index, rounded_clips,
                                        MAXIMUM_ROUNDED_CLIPS)
        : overflow_rounded_clips(cache->layout, index, rounded_clips,
                                 MAXIMUM_ROUNDED_CLIPS);
    int visible_left = clip_left > 0 ? clip_left : 0;
    int visible_right = clip_right < viewport_width
                        ? clip_right : viewport_width;
    int visible_top = clip_top > scroll_y ? clip_top : scroll_y;
    int visible_bottom = clip_bottom < scroll_y + viewport_height
                         ? clip_bottom : scroll_y + viewport_height;
    if (visible_right <= visible_left || visible_bottom <= visible_top
        || !intersects(&command, visible_left, visible_top,
                       visible_right, visible_bottom)) return;
    RasterTarget target = {
        .pixels = frame,
        .stride = (size_t) viewport_width,
        .origin_x = 0,
        .origin_y = scroll_y,
        .width = viewport_width,
        .height = viewport_height,
        .left = visible_left,
        .top = visible_top,
        .right = visible_right,
        .bottom = visible_bottom,
        .rounded_clips = rounded_clips,
        .rounded_clip_count = rounded_clip_count
    };
    rasterize_command(cache, &target, &command);
    cache->overflow_direct_commands++;
}

typedef struct {
    const uint32_t *orders;
    size_t at;
    size_t end;
} PaintOrderCursor;

static bool has_active_vertical_overflow_scroll(const LayoutDocument *layout)
{
    for (size_t i = 0; i < layout->node_box_count; i++) {
        if (layout->node_boxes[i].clips_y
            && layout->node_boxes[i].scroll_y != 0) return true;
    }
    return false;
}

static void paint_overlay_command(TileCache *cache, uint16_t *frame,
                                  const DrawCommand *command,
                                  int scroll_y, int viewport_width,
                                  int viewport_height);

static void paint_overflow_commands(TileCache *cache, uint16_t *frame,
                                    int scroll_y, int viewport_width,
                                    int viewport_height)
{
    if (cache->layout->command_flags != NULL) {
        (void) overflow_cache_prepare(cache);
    }
    if (cache->layout->command_flags == NULL
        || cache->layout->spatial_band_offsets == NULL) {
        for (size_t order = 0; order < cache->layout->count; order++) {
            cache->command_candidates++;
            size_t index = cache->layout->paint_order_count
                           == cache->layout->count
                ? cache->layout->paint_order[order] : order;
            if (command_in_overflow(cache->layout, index)) {
                paint_overflow_command(cache, frame, index, scroll_y,
                                       viewport_width, viewport_height);
            }
        }
        return;
    }
    PaintOrderCursor cursors[40];
    size_t cursor_count = 0;
    int first_band = scroll_y / TILEFINCH_TILE_SIZE;
    int last_band = (scroll_y + viewport_height - 1) / TILEFINCH_TILE_SIZE;
    if (first_band < 0) first_band = 0;
    if (last_band >= (int) cache->layout->spatial_band_count) {
        last_band = (int) cache->layout->spatial_band_count - 1;
    }
    if (last_band - first_band + 1 > 32) {
        for (size_t order = 0; order < cache->layout->count; order++) {
            size_t index = cache->layout->paint_order[order];
            cache->command_candidates++;
            if (cache->layout->command_flags[index]
                & LAYOUT_COMMAND_OVERFLOW) {
                paint_overflow_command(cache, frame, index, scroll_y,
                                       viewport_width, viewport_height);
            } else if (cache->layout->command_flags[index]
                       & LAYOUT_COMMAND_LATE_POSITIONED) {
                paint_overlay_command(
                    cache, frame, &cache->layout->commands[index],
                    scroll_y, viewport_width, viewport_height);
            }
        }
        return;
    }
    for (int band = first_band; band <= last_band; band++) {
        size_t start = cache->layout->spatial_band_offsets[band];
        size_t end = cache->layout->spatial_band_offsets[band + 1];
        if (start != end) cursors[cursor_count++] = (PaintOrderCursor) {
            cache->layout->spatial_band_orders, start, end};
    }
    if (cache->layout->spatial_global_count != 0) {
        cursors[cursor_count++] = (PaintOrderCursor) {
            cache->layout->spatial_global_orders, 0,
            cache->layout->spatial_global_count};
    }
    if (cache->layout->overflow_order_count != 0
        && has_active_vertical_overflow_scroll(cache->layout)) {
        cursors[cursor_count++] = (PaintOrderCursor) {
            cache->layout->overflow_orders, 0,
            cache->layout->overflow_order_count};
    }
    if (cache->layout->late_positioned_order_count != 0) {
        cursors[cursor_count++] = (PaintOrderCursor) {
            cache->layout->late_positioned_orders, 0,
            cache->layout->late_positioned_order_count};
    }
    while (true) {
        uint32_t next = UINT32_MAX;
        for (size_t cursor = 0; cursor < cursor_count; cursor++) {
            if (cursors[cursor].at < cursors[cursor].end
                && cursors[cursor].orders[cursors[cursor].at] < next) {
                next = cursors[cursor].orders[cursors[cursor].at];
            }
        }
        if (next == UINT32_MAX) break;
        for (size_t cursor = 0; cursor < cursor_count; cursor++) {
            while (cursors[cursor].at < cursors[cursor].end
                   && cursors[cursor].orders[cursors[cursor].at] == next) {
                cursors[cursor].at++;
            }
        }
        size_t index = cache->layout->paint_order[next];
        cache->command_candidates++;
        if (cache->layout->command_flags[index]
            & LAYOUT_COMMAND_OVERFLOW) {
            paint_overflow_command(cache, frame, index, scroll_y,
                                   viewport_width, viewport_height);
        } else if (cache->layout->command_flags[index]
                   & LAYOUT_COMMAND_LATE_POSITIONED) {
            paint_overlay_command(
                cache, frame, &cache->layout->commands[index],
                scroll_y, viewport_width, viewport_height);
        }
    }
}
static void paint_overlay_command(TileCache *cache, uint16_t *frame,
                                  const DrawCommand *command,
                                  int scroll_y, int viewport_width,
                                  int viewport_height)
{
    if (!intersects(command, 0, scroll_y, viewport_width,
                    scroll_y + viewport_height)) return;
    RasterTarget target = {
        .pixels = frame,
        .stride = (size_t) viewport_width,
        .origin_x = 0,
        .origin_y = scroll_y,
        .width = viewport_width,
        .height = viewport_height,
        .left = 0,
        .top = scroll_y,
        .right = viewport_width,
        .bottom = scroll_y + viewport_height
    };
    rasterize_command(cache, &target, command);
}

bool tile_cache_set_frame(TileCache *cache, uint16_t *frame,
                          size_t frame_pixels)
{
    if (cache == NULL || frame == NULL || frame_pixels == 0) return false;
    cache->frame = frame;
    cache->frame_pixels = frame_pixels;
    return true;
}

void tile_cache_destroy(TileCache *cache)
{
    if (cache == NULL) return;
    if (cache->budget != NULL) {
        overflow_cache_destroy(cache);
        budget_free(cache->budget, cache->gradient_cache);
        glyph_cache_clear(cache, false);
        scaled_image_clear(cache);
        decoded_image_clear(cache);
        canvas_overlay_cache_clear(cache);
        budget_free(cache->budget, cache->fixed_row_last);
        budget_free(cache->budget, cache->fixed_row_first);
        budget_free(cache->budget, cache->fixed_alpha);
        budget_free(cache->budget, cache->fixed_pixels);
        budget_free(cache->budget, cache->frame_scratch_tile);
        budget_free(cache->budget, cache->glyph_cache);
        budget_free(cache->budget, cache->tiles);
        tile_cache_release_visual_layout(cache);
    }
    memset(cache, 0, sizeof(*cache));
}

static bool fixed_screen_command(const TileCache *cache,
                                 const FixedRange *range, size_t index,
                                 int viewport_height, DrawCommand *command)
{
    if (index < range->command_start || index >= range->command_end
        || index >= cache->layout->count) return false;
    *command = cache->layout->commands[index];
    int target_y = range->from_bottom
        ? render_saturating_subtract_int(
              render_saturating_subtract_int(
                  viewport_height, range->inset), range->height)
        : range->inset;
    command->y = render_saturating_add_int(
        command->y,
        render_saturating_subtract_int(target_y, range->origin_y));
    return true;
}

static bool fixed_range_owns_command(const LayoutDocument *layout,
                                     size_t range_index,
                                     size_t command_index)
{
    if (layout == NULL || range_index >= layout->fixed_count) return false;
    const FixedRange *candidate = &layout->fixed_ranges[range_index];
    if (command_index < candidate->command_start
        || command_index >= candidate->command_end) return false;
    size_t candidate_span = candidate->command_end - candidate->command_start;
    for (size_t i = 0; i < layout->fixed_count; i++) {
        if (i == range_index) continue;
        const FixedRange *other = &layout->fixed_ranges[i];
        if (command_index < other->command_start
            || command_index >= other->command_end) continue;
        size_t other_span = other->command_end - other->command_start;
        if (other_span < candidate_span
            || (other_span == candidate_span && i < range_index)) {
            return false;
        }
    }
    return true;
}

typedef struct {
    DrawCommand command;
    int clip_left;
    int clip_top;
    int clip_right;
    int clip_bottom;
    RoundedClip rounded_clips[MAXIMUM_ROUNDED_CLIPS];
    size_t rounded_clip_count;
} FixedScreenCommand;

static bool fixed_screen_command_geometry(
    TileCache *cache, const FixedRange *range, size_t index,
    int viewport_width, int viewport_height, FixedScreenCommand *fixed)
{
    if (fixed == NULL || !fixed_screen_command(
            cache, range, index, viewport_height, &fixed->command)) {
        return false;
    }
    fixed->clip_left = 0;
    fixed->clip_top = 0;
    fixed->clip_right = viewport_width;
    fixed->clip_bottom = viewport_height;
    fixed->rounded_clip_count = 0;
    if (cache->layout->command_flags == NULL
        || !(cache->layout->command_flags[index]
             & LAYOUT_COMMAND_OVERFLOW)) {
        return intersects(&fixed->command, 0, 0,
                          viewport_width, viewport_height);
    }

    int dx = 0, dy = 0, clip_left = 0, clip_top = 0;
    int clip_right = 0, clip_bottom = 0;
    const OverflowGeometry *geometry = overflow_cached_geometry(cache, index);
    if (geometry != NULL) {
        if (!geometry->found
            || geometry->clip_right <= geometry->clip_left
            || geometry->clip_bottom <= geometry->clip_top) return false;
        dx = geometry->dx;
        dy = geometry->dy;
        clip_left = geometry->clip_left;
        clip_top = geometry->clip_top;
        clip_right = geometry->clip_right;
        clip_bottom = geometry->clip_bottom;
        fixed->rounded_clip_count = overflow_cached_rounded_clips(
            cache, index, fixed->rounded_clips, MAXIMUM_ROUNDED_CLIPS);
    } else {
        if (!overflow_command_geometry(
                cache->layout, index, &dx, &dy, &clip_left, &clip_top,
                &clip_right, &clip_bottom)) return false;
        fixed->rounded_clip_count = overflow_rounded_clips(
            cache->layout, index, fixed->rounded_clips,
            MAXIMUM_ROUNDED_CLIPS);
    }

    int fixed_target_y = range->from_bottom
        ? render_saturating_subtract_int(
              render_saturating_subtract_int(
                  viewport_height, range->inset), range->height)
        : range->inset;
    int fixed_dy = render_saturating_subtract_int(
        fixed_target_y, range->origin_y);
    fixed->command.x = render_saturating_add_int(fixed->command.x, dx);
    fixed->command.y = render_saturating_add_int(fixed->command.y, dy);
    if (clip_left > fixed->clip_left) fixed->clip_left = clip_left;
    if (clip_right < fixed->clip_right) fixed->clip_right = clip_right;
    clip_top = render_saturating_add_int(clip_top, fixed_dy);
    clip_bottom = render_saturating_add_int(clip_bottom, fixed_dy);
    if (clip_top > fixed->clip_top) fixed->clip_top = clip_top;
    if (clip_bottom < fixed->clip_bottom) fixed->clip_bottom = clip_bottom;
    for (size_t i = 0; i < fixed->rounded_clip_count; i++) {
        fixed->rounded_clips[i].y = render_saturating_add_int(
            fixed->rounded_clips[i].y, fixed_dy);
    }
    return fixed->clip_right > fixed->clip_left
        && fixed->clip_bottom > fixed->clip_top
        && intersects(&fixed->command, fixed->clip_left, fixed->clip_top,
                      fixed->clip_right, fixed->clip_bottom);
}

static bool layout_has_fixed_backdrop(const LayoutDocument *layout,
                                      int viewport_width,
                                      int viewport_height);

static bool layout_has_css_backdrop_filter(const LayoutDocument *layout)
{
    if (layout == NULL) return false;
    for (size_t range_index = 0; range_index < layout->fixed_count;
         range_index++) {
        const FixedRange *range = &layout->fixed_ranges[range_index];
        size_t end = range->command_end < layout->count
            ? range->command_end : layout->count;
        for (size_t i = range->command_start; i < end; i++) {
            if (draw_command_backdrop_blur(&layout->commands[i]) != 0u)
                return true;
        }
    }
    return false;
}

static bool layout_has_unbounded_css_backdrop_filter(
    const LayoutDocument *layout)
{
    if (layout == NULL) return false;
    for (size_t range_index = 0; range_index < layout->fixed_count;
         range_index++) {
        const FixedRange *range = &layout->fixed_ranges[range_index];
        if (range->scroll_end != INT_MAX) continue;
        size_t end = range->command_end < layout->count
            ? range->command_end : layout->count;
        for (size_t i = range->command_start; i < end; i++) {
            if (draw_command_backdrop_blur(&layout->commands[i]) != 0u)
                return true;
        }
    }
    return false;
}

static bool build_fixed_cache(TileCache *cache, int viewport_width,
                              int viewport_height)
{
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_FIXED_CACHE") != NULL) return false;
#endif
    if (cache->layout->fixed_count == 0) return true;
    /* A backdrop filter samples the already-composited page.  The ordinary
       fixed cache is deliberately page-independent, so route only these rare
       bounded commands through the direct framebuffer path. */
    if (layout_has_unbounded_css_backdrop_filter(cache->layout)) return false;
    if (cache->layout->command_flags != NULL) {
        (void) overflow_cache_prepare(cache);
    }
    if (cache->fixed_ready
        && cache->fixed_viewport_width == viewport_width
        && cache->fixed_viewport_height == viewport_height) return true;

    budget_free(cache->budget, cache->fixed_alpha);
    budget_free(cache->budget, cache->fixed_pixels);
    budget_free(cache->budget, cache->fixed_row_last);
    budget_free(cache->budget, cache->fixed_row_first);
    cache->fixed_alpha = NULL;
    cache->fixed_pixels = NULL;
    cache->fixed_row_first = NULL;
    cache->fixed_row_last = NULL;
    cache->fixed_ready = false;
    cache->fixed_backdrop = false;
    cache->fixed_backdrop_masked = false;
    cache->fixed_cache_bytes = 0;
    int left = viewport_width, top = viewport_height, right = 0, bottom = 0;
    for (size_t range_index = 0;
        range_index < cache->layout->fixed_count; range_index++) {
        const FixedRange *range = &cache->layout->fixed_ranges[range_index];
        if (range->scroll_end != INT_MAX) continue;
        for (size_t order = 0; order < cache->layout->count; order++) {
            size_t i = cache->layout->paint_order_count == cache->layout->count
                       ? cache->layout->paint_order[order] : order;
            if (!fixed_range_owns_command(
                    cache->layout, range_index, i)) continue;
            FixedScreenCommand fixed;
            if (!fixed_screen_command_geometry(
                    cache, range, i, viewport_width, viewport_height,
                    &fixed)) continue;
            int command_left = fixed.command.x > fixed.clip_left
                               ? fixed.command.x : fixed.clip_left;
            int command_top = fixed.command.y > fixed.clip_top
                              ? fixed.command.y : fixed.clip_top;
            int command_right = fixed.command.x + fixed.command.width;
            int command_bottom = fixed.command.y + fixed.command.height;
            if (command_right > fixed.clip_right) {
                command_right = fixed.clip_right;
            }
            if (command_bottom > fixed.clip_bottom) {
                command_bottom = fixed.clip_bottom;
            }
            if (command_left < left) left = command_left;
            if (command_top < top) top = command_top;
            if (command_right > right) right = command_right;
            if (command_bottom > bottom) bottom = command_bottom;
        }
    }
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > viewport_width) right = viewport_width;
    if (bottom > viewport_height) bottom = viewport_height;
    if (right <= left || bottom <= top) {
        cache->fixed_ready = true;
        cache->fixed_viewport_width = viewport_width;
        cache->fixed_viewport_height = viewport_height;
        return true;
    }
    size_t width = (size_t) (right - left);
    size_t height = (size_t) (bottom - top);
    if (width > UINT16_MAX || height > SIZE_MAX / width) return false;
    size_t pixels = width * height;
    if (pixels > 512u * 1024u / 3u) return false;
    cache->fixed_pixels = budget_calloc(cache->budget, pixels,
                                        sizeof(*cache->fixed_pixels));
    cache->fixed_alpha = budget_calloc(cache->budget, pixels,
                                       sizeof(*cache->fixed_alpha));
    cache->fixed_row_first = budget_malloc(
        cache->budget, height * sizeof(*cache->fixed_row_first));
    cache->fixed_row_last = budget_malloc(
        cache->budget, height * sizeof(*cache->fixed_row_last));
    if (cache->fixed_pixels == NULL || cache->fixed_alpha == NULL
        || cache->fixed_row_first == NULL || cache->fixed_row_last == NULL) {
        budget_free(cache->budget, cache->fixed_row_last);
        budget_free(cache->budget, cache->fixed_row_first);
        budget_free(cache->budget, cache->fixed_alpha);
        budget_free(cache->budget, cache->fixed_pixels);
        cache->fixed_alpha = NULL;
        cache->fixed_pixels = NULL;
        cache->fixed_row_first = NULL;
        cache->fixed_row_last = NULL;
        return false;
    }
    cache->fixed_left = left;
    cache->fixed_top = top;
    cache->fixed_width = (int) width;
    cache->fixed_height = (int) height;
    RasterTarget target = {
        .pixels = cache->fixed_pixels,
        .alpha = cache->fixed_alpha,
        .stride = width,
        .origin_x = left,
        .origin_y = top,
        .width = (int) width,
        .height = (int) height,
        .left = left,
        .top = top,
        .right = right,
        .bottom = bottom
    };

    for (size_t range_index = 0;
        range_index < cache->layout->fixed_count; range_index++) {
        const FixedRange *range = &cache->layout->fixed_ranges[range_index];
        if (range->scroll_end != INT_MAX) continue;
        for (size_t order = 0; order < cache->layout->count; order++) {
            size_t i = cache->layout->paint_order_count == cache->layout->count
                       ? cache->layout->paint_order[order] : order;
            if (!fixed_range_owns_command(
                    cache->layout, range_index, i)) continue;
            FixedScreenCommand fixed;
            if (!fixed_screen_command_geometry(
                    cache, range, i, viewport_width, viewport_height,
                    &fixed)) continue;
            RasterTarget clipped = target;
            if (fixed.clip_left > clipped.left) {
                clipped.left = fixed.clip_left;
            }
            if (fixed.clip_top > clipped.top) clipped.top = fixed.clip_top;
            if (fixed.clip_right < clipped.right) {
                clipped.right = fixed.clip_right;
            }
            if (fixed.clip_bottom < clipped.bottom) {
                clipped.bottom = fixed.clip_bottom;
            }
            clipped.rounded_clips = fixed.rounded_clips;
            clipped.rounded_clip_count = fixed.rounded_clip_count;
            rasterize_command(cache, &clipped, &fixed.command);
        }
    }
    for (size_t y = 0; y < height; y++) {
        uint16_t first = UINT16_MAX;
        uint16_t last = 0;
        for (size_t x = 0; x < width; x++) {
            if (cache->fixed_alpha[y * width + x] == 0) continue;
            if (first == UINT16_MAX) first = (uint16_t) x;
            last = (uint16_t) x;
        }
        cache->fixed_row_first[y] = first;
        cache->fixed_row_last[y] = last;
    }
    cache->fixed_ready = true;
    size_t cacheable_ranges = 0;
    for (size_t i = 0; i < cache->layout->fixed_count; i++) {
        if (cache->layout->fixed_ranges[i].scroll_end == INT_MAX)
            cacheable_ranges++;
    }
    cache->fixed_backdrop = cacheable_ranges == 1
        && layout_has_fixed_backdrop(
               cache->layout, viewport_width, viewport_height);
    cache->fixed_viewport_width = viewport_width;
    cache->fixed_viewport_height = viewport_height;
    cache->fixed_cache_builds++;
    cache->fixed_cache_bytes = pixels * (sizeof(*cache->fixed_pixels)
                                         + sizeof(*cache->fixed_alpha))
                               + height * (sizeof(*cache->fixed_row_first)
                                           + sizeof(*cache->fixed_row_last));
    return true;
}

static void paint_cached_fixed_overlays(TileCache *cache, uint16_t *frame,
                                        int viewport_width,
                                        int viewport_height)
{
    if (!cache->fixed_ready || cache->fixed_pixels == NULL
        || cache->fixed_alpha == NULL) return;
    /* The root canvas can be covered by an opaque body background (a dark
       hero shell is one example), so LayoutDocument::page_background
       is not necessarily the color visible beneath the staged backdrop.
       The top-left pixel is already the fully rendered immutable page and is
       the most reliable allocation-free canvas sample for this narrow path. */
    uint16_t canvas = frame[0];
    bool mask_backdrop = cache->fixed_backdrop
        && !cache->fixed_backdrop_masked;
    for (int y = 0; y < cache->fixed_height; y++) {
        int screen_y = cache->fixed_top + y;
        if (screen_y < 0 || screen_y >= viewport_height) continue;
        uint16_t first = cache->fixed_row_first[y];
        if (first == UINT16_MAX) continue;
        uint16_t last = cache->fixed_row_last[y];
        int content_first = cache->fixed_width;
        int content_last = -1;
        if (mask_backdrop) {
            /* The immutable overlay has no pixels outside [first,last], so
               inspecting the rest of the viewport cannot affect output.
               Discover the content span before touching the alpha plane,
               then fold masking into the normal blit pass below. */
            for (int x = first; x <= last; x++) {
                int screen_x = cache->fixed_left + x;
                if (screen_x < 0 || screen_x >= viewport_width) continue;
                size_t destination =
                    (size_t) screen_y * viewport_width + screen_x;
                if (frame[destination] == canvas) continue;
                if (x < content_first) content_first = x;
                if (x > content_last) content_last = x;
            }
            if (content_last >= content_first) {
                if (content_first > first) content_first--;
                if (content_last < last) content_last++;
            }
        }
        for (int x = first; x <= last; x++) {
            int screen_x = cache->fixed_left + x;
            if (screen_x < 0 || screen_x >= viewport_width) continue;
            size_t source = (size_t) y * cache->fixed_width + x;
            if (x >= content_first && x <= content_last) {
                cache->fixed_alpha[source] = 0;
                continue;
            }
            unsigned alpha = cache->fixed_alpha[source];
            if (alpha == 0) continue;
            size_t destination = (size_t) screen_y * viewport_width + screen_x;
            if (alpha == 255) {
                frame[destination] = cache->fixed_pixels[source];
            } else {
                uint16_t color = cache->fixed_pixels[source];
                uint32_t rgb =
                    (tilefinch_rgb5_to_u8(
                         tilefinch_rgb565_red_code(color)) << 16)
                    | (tilefinch_rgb6_to_u8(
                           tilefinch_rgb565_green_code(color)) << 8)
                    | tilefinch_rgb5_to_u8(
                        tilefinch_rgb565_blue_code(color));
                frame[destination] = blend_rgb565(frame[destination], rgb,
                                                   alpha);
            }
            cache->fixed_cache_pixels++;
        }
    }
    if (mask_backdrop) cache->fixed_backdrop_masked = true;
    cache->fixed_cache_blits++;
}

static void paint_sticky_overlays(TileCache *cache, uint16_t *frame,
                                  int scroll_y, int viewport_width,
                                  int viewport_height)
{
    if (scroll_y <= 0) return;
    for (size_t range_index = 0;
         range_index < cache->layout->sticky_count; range_index++) {
        const StickyRange *range = &cache->layout->sticky_ranges[range_index];
        int trigger = render_saturating_subtract_int(
            range->origin_y, range->top);
        if (scroll_y <= trigger) continue;
        for (size_t order = 0; order < cache->layout->count; order++) {
            size_t i = cache->layout->paint_order_count == cache->layout->count
                       ? cache->layout->paint_order[order] : order;
            if (i < range->command_start || i >= range->command_end) continue;
            const DrawCommand *source = &cache->layout->commands[i];
            DrawCommand command = *source;
            int sticky_dy = render_saturating_subtract_int(scroll_y, trigger);
            if (range->maximum_offset != INT_MAX
                && sticky_dy > range->maximum_offset) {
                sticky_dy = range->maximum_offset;
            }
            command.y = render_saturating_add_int(command.y, sticky_dy);
            if (cache->layout->command_flags == NULL
                || !(cache->layout->command_flags[i]
                     & LAYOUT_COMMAND_OVERFLOW)) {
                paint_overlay_command(cache, frame, &command, scroll_y,
                                      viewport_width, viewport_height);
                continue;
            }

            int dx = 0, dy = 0, clip_left = 0, clip_top = 0;
            int clip_right = 0, clip_bottom = 0;
            const OverflowGeometry *geometry =
                overflow_cached_geometry(cache, i);
            if (geometry != NULL) {
                if (!geometry->found
                    || geometry->clip_right <= geometry->clip_left
                    || geometry->clip_bottom <= geometry->clip_top) {
                    continue;
                }
                dx = geometry->dx;
                dy = geometry->dy;
                clip_left = geometry->clip_left;
                clip_top = geometry->clip_top;
                clip_right = geometry->clip_right;
                clip_bottom = geometry->clip_bottom;
            } else if (!overflow_command_geometry(
                           cache->layout, i, &dx, &dy, &clip_left, &clip_top,
                           &clip_right, &clip_bottom)) {
                continue;
            }
            command.x = render_saturating_add_int(command.x, dx);
            command.y = render_saturating_add_int(command.y, dy);
            clip_top = render_saturating_add_int(clip_top, sticky_dy);
            clip_bottom = render_saturating_add_int(clip_bottom, sticky_dy);
            RoundedClip rounded_clips[MAXIMUM_ROUNDED_CLIPS];
            size_t rounded_clip_count = geometry != NULL
                ? overflow_cached_rounded_clips(
                    cache, i, rounded_clips, MAXIMUM_ROUNDED_CLIPS)
                : overflow_rounded_clips(
                    cache->layout, i, rounded_clips, MAXIMUM_ROUNDED_CLIPS);
            for (size_t clip = 0; clip < rounded_clip_count; clip++) {
                rounded_clips[clip].y = render_saturating_add_int(
                    rounded_clips[clip].y, sticky_dy);
            }
            int visible_left = clip_left > 0 ? clip_left : 0;
            int visible_right = clip_right < viewport_width
                ? clip_right : viewport_width;
            int visible_top = clip_top > scroll_y ? clip_top : scroll_y;
            int viewport_bottom = render_saturating_add_int(
                scroll_y, viewport_height);
            int visible_bottom = clip_bottom < viewport_bottom
                ? clip_bottom : viewport_bottom;
            if (visible_right <= visible_left
                || visible_bottom <= visible_top
                || !intersects(&command, visible_left, visible_top,
                               visible_right, visible_bottom)) {
                continue;
            }
            RasterTarget target = {
                .pixels = frame,
                .stride = (size_t) viewport_width,
                .origin_x = 0,
                .origin_y = scroll_y,
                .width = viewport_width,
                .height = viewport_height,
                .left = visible_left,
                .top = visible_top,
                .right = visible_right,
                .bottom = visible_bottom,
                .rounded_clips = rounded_clips,
                .rounded_clip_count = rounded_clip_count
            };
            rasterize_command(cache, &target, &command);
        }
    }
}

static void paint_fixed_overlays(TileCache *cache, uint16_t *frame,
                                 int scroll_y, int viewport_width,
                                 int viewport_height, bool bounded_only)
{
    RasterTarget target = {
        .pixels = frame,
        .stride = (size_t) viewport_width,
        .origin_x = 0,
        .origin_y = 0,
        .width = viewport_width,
        .height = viewport_height,
        .left = 0,
        .top = 0,
        .right = viewport_width,
        .bottom = viewport_height
    };
    for (size_t range_index = 0;
        range_index < cache->layout->fixed_count; range_index++) {
        const FixedRange *range = &cache->layout->fixed_ranges[range_index];
        if (bounded_only && range->scroll_end == INT_MAX) continue;
        if (range->scroll_end != INT_MAX
            && scroll_y >= range->scroll_end) continue;
        for (size_t order = 0; order < cache->layout->count; order++) {
            size_t i = cache->layout->paint_order_count == cache->layout->count
                       ? cache->layout->paint_order[order] : order;
            if (!fixed_range_owns_command(
                    cache->layout, range_index, i)) continue;
            FixedScreenCommand fixed;
            if (!fixed_screen_command_geometry(
                    cache, range, i, viewport_width, viewport_height,
                    &fixed)) continue;
            RasterTarget clipped = target;
            clipped.left = fixed.clip_left;
            clipped.top = fixed.clip_top;
            clipped.right = fixed.clip_right;
            clipped.bottom = fixed.clip_bottom;
            clipped.rounded_clips = fixed.rounded_clips;
            clipped.rounded_clip_count = fixed.rounded_clip_count;
            rasterize_command(cache, &clipped, &fixed.command);
        }
    }
}

static bool layout_has_fixed_backdrop(const LayoutDocument *layout,
                                      int viewport_width,
                                      int viewport_height)
{
    if (layout == NULL) return false;
    for (size_t range_index = 0; range_index < layout->fixed_count;
         range_index++) {
        const FixedRange *range = &layout->fixed_ranges[range_index];
        if (range->scroll_end != INT_MAX) continue;
        size_t end = range->command_end < layout->count
            ? range->command_end : layout->count;
        for (size_t i = range->command_start; i < end; i++) {
            const DrawCommand *command = &layout->commands[i];
            if (command->type == DRAW_IMAGE && command->z_index < 0
                && command->width >= viewport_width
                && command->height >= viewport_height) return true;
        }
    }
    return false;
}

static void paint_scroll_indicator(const TileCache *cache, uint16_t *frame,
                                   int scroll_y, int viewport_width,
                                   int viewport_height)
{
    if (scroll_y <= 0 || cache->layout->height <= viewport_height
        || viewport_width < 8 || viewport_height < 20) return;
    int thumb_height = viewport_height * viewport_height
                       / cache->layout->height;
    if (thumb_height < 20) thumb_height = 20;
    if (thumb_height > viewport_height) thumb_height = viewport_height;
    int maximum_scroll = cache->layout->height - viewport_height;
    int thumb_y = scroll_y * (viewport_height - thumb_height)
                  / maximum_scroll;
    int thumb_x = viewport_width - 8;
    uint16_t color = rgb565(0x888888);
    for (int y = 0; y < thumb_height; y++) {
        for (int x = 0; x < 7; x++) {
            if ((y == 0 || y == thumb_height - 1) && (x < 2 || x > 4)) {
                continue;
            }
            frame[(size_t) (thumb_y + y) * viewport_width
                  + thumb_x + x] = color;
        }
    }
}

bool render_write_frame_ppm(const char *path, const uint16_t *frame,
                            size_t frame_pixels, int width, int height)
{
    if (path == NULL || frame == NULL || width <= 0 || height <= 0
        || (size_t) width > SIZE_MAX / (size_t) height) return false;
    size_t pixels = (size_t) width * (size_t) height;
    if (pixels > frame_pixels) return false;
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    /* Convert in bounded chunks and write each with one fwrite; the former
       three fputc calls per pixel dominated capture cost. */
    enum { RENDER_PPM_CHUNK_PIXELS = 1024 };
    unsigned char chunk[RENDER_PPM_CHUNK_PIXELS * 3];
    for (size_t at = 0; at < pixels;) {
        size_t batch = pixels - at < RENDER_PPM_CHUNK_PIXELS
                       ? pixels - at : RENDER_PPM_CHUNK_PIXELS;
        unsigned char *out = chunk;
        for (size_t i = 0; i < batch; i++) {
            uint16_t value = frame[at + i];
            *out++ = (unsigned char) (
                tilefinch_rgb565_red_code(value) * 255u / 31u);
            *out++ = (unsigned char) (
                tilefinch_rgb565_green_code(value) * 255u / 63u);
            *out++ = (unsigned char) (
                tilefinch_rgb565_blue_code(value) * 255u / 31u);
        }
        if (fwrite(chunk, 1, batch * 3, file) != batch * 3) break;
        at += batch;
    }
    bool ok = ferror(file) == 0;
    return fclose(file) == 0 && ok;
}

static void focus_pixel(uint16_t *frame, int viewport_width,
                        int viewport_height, int x, int y, uint16_t color)
{
    if (x >= 0 && y >= 0 && x < viewport_width && y < viewport_height) {
        frame[(size_t) y * viewport_width + x] = color;
    }
}

void render_paint_focus_outline(uint16_t *frame, size_t frame_pixels,
                                int viewport_width, int viewport_height,
                                int x, int y, int width, int height)
{
    if (frame == NULL || viewport_width <= 0 || viewport_height <= 0
        || frame_pixels < (size_t) viewport_width * viewport_height
        || width <= 0 || height <= 0) return;
    int left = x - 2, top = y - 2;
    int right = x + width + 1, bottom = y + height + 1;
    uint16_t white = rgb565(0xffffff);
    uint16_t blue = rgb565(0x0a84ff);
    for (int px = left; px <= right; px++) {
        focus_pixel(frame, viewport_width, viewport_height, px, top, white);
        focus_pixel(frame, viewport_width, viewport_height, px, bottom, white);
        focus_pixel(frame, viewport_width, viewport_height, px, top + 1, blue);
        focus_pixel(frame, viewport_width, viewport_height, px, bottom - 1,
                    blue);
    }
    for (int py = top; py <= bottom; py++) {
        focus_pixel(frame, viewport_width, viewport_height, left, py, white);
        focus_pixel(frame, viewport_width, viewport_height, right, py, white);
        focus_pixel(frame, viewport_width, viewport_height, left + 1, py, blue);
        focus_pixel(frame, viewport_width, viewport_height, right - 1, py,
                    blue);
    }
}

static bool focus_outline_pattern(
    unsigned style, int position, int thickness)
{
    if (style == STYLE_OUTLINE_SOLID) return true;
    int unit = thickness > 0 ? thickness : 1;
    if (style == STYLE_OUTLINE_DASHED) unit *= 3;
    return ((position / unit) & 1) == 0;
}

static void paint_authored_focus_pixel(
    uint16_t *frame, int viewport_width, int viewport_height,
    int x, int y, uint32_t color, unsigned alpha)
{
    if (x < 0 || y < 0 || x >= viewport_width || y >= viewport_height
        || alpha == 0) return;
    size_t index = (size_t) y * (size_t) viewport_width + (size_t) x;
    frame[index] = alpha == 255u
        ? rgb565(color) : blend_rgb565(frame[index], color, alpha);
}

void render_paint_authored_focus_outline(
    uint16_t *frame, size_t frame_pixels,
    int viewport_width, int viewport_height,
    int x, int y, int width, int height,
    int outline_width, int outline_offset, unsigned outline_style,
    uint32_t color, uint8_t alpha)
{
    if (frame == NULL || viewport_width <= 0 || viewport_height <= 0
        || frame_pixels < (size_t) viewport_width * viewport_height
        || width <= 0 || height <= 0 || outline_width <= 0
        || outline_style == STYLE_OUTLINE_NONE || alpha == 0) return;
    if (outline_width > 16) outline_width = 16;
    int outset = outline_offset + outline_width;
    int left = x - outset, top = y - outset;
    int right = x + width + outset - 1;
    int bottom = y + height + outset - 1;
    for (int line = 0; line < outline_width; line++) {
        int line_left = left + line, line_top = top + line;
        int line_right = right - line, line_bottom = bottom - line;
        if (line_left > line_right || line_top > line_bottom) break;
        for (int px = line_left; px <= line_right; px++) {
            int position = px - line_left;
            if (!focus_outline_pattern(
                    outline_style, position, outline_width)) continue;
            paint_authored_focus_pixel(
                frame, viewport_width, viewport_height,
                px, line_top, color, alpha);
            paint_authored_focus_pixel(
                frame, viewport_width, viewport_height,
                px, line_bottom, color, alpha);
        }
        for (int py = line_top + 1; py < line_bottom; py++) {
            int position = py - line_top;
            if (!focus_outline_pattern(
                    outline_style, position, outline_width)) continue;
            paint_authored_focus_pixel(
                frame, viewport_width, viewport_height,
                line_left, py, color, alpha);
            paint_authored_focus_pixel(
                frame, viewport_width, viewport_height,
                line_right, py, color, alpha);
        }
    }
}

void render_paint_find_highlight(
    uint16_t *frame, size_t frame_pixels,
    int viewport_width, int viewport_height,
    int x, int y, int width, int height, bool current)
{
    if (frame == NULL || viewport_width <= 0 || viewport_height <= 0
        || frame_pixels < (size_t) viewport_width * viewport_height
        || width <= 0 || height <= 0) return;
    int left = x < 0 ? 0 : x;
    int top = y < 0 ? 0 : y;
    int64_t right_wide = (int64_t) x + (int64_t) width;
    int64_t bottom_wide = (int64_t) y + (int64_t) height;
    int right = right_wide > viewport_width
        ? viewport_width : (int) right_wide;
    int bottom = bottom_wide > viewport_height
        ? viewport_height : (int) bottom_wide;
    if (left >= right || top >= bottom) return;
    uint32_t fill = current ? UINT32_C(0xff9f0a) : UINT32_C(0xffd60a);
    unsigned alpha = current ? 112u : 72u;
    uint16_t border = rgb565(current ? UINT32_C(0xff7a00)
                                     : UINT32_C(0xd6a900));
    for (int py = top; py < bottom; py++) {
        size_t row = (size_t) py * (size_t) viewport_width;
        for (int px = left; px < right; px++) {
            size_t index = row + (size_t) px;
            frame[index] = blend_rgb565(frame[index], fill, alpha);
        }
    }
    if (!current) return;
    for (int px = left; px < right; px++) {
        frame[(size_t) top * (size_t) viewport_width + (size_t) px] = border;
        frame[(size_t) (bottom - 1) * (size_t) viewport_width
              + (size_t) px] = border;
    }
    for (int py = top; py < bottom; py++) {
        frame[(size_t) py * (size_t) viewport_width + (size_t) left] = border;
        frame[(size_t) py * (size_t) viewport_width
              + (size_t) (right - 1)] = border;
    }
}

typedef struct {
    DrawCommand command;
    const ImageResource *image;
    size_t command_index;
    size_t paint_order;
    int clip_left;
    int clip_top;
    int clip_right;
    int clip_bottom;
} CanvasFastFrameCandidate;

static bool canvas_surface_is_opaque(const ImageResource *image)
{
    if (image == NULL || !image->is_canvas || image->pixels == NULL
        || image->width <= 0 || image->height <= 0
        || (size_t) image->width > SIZE_MAX / (size_t) image->height
        || (size_t) image->width * (size_t) image->height
               > SIZE_MAX / 4u) {
        return false;
    }
    size_t pixels = (size_t) image->width * (size_t) image->height;
    for (size_t i = 0; i < pixels; i++) {
        if (image->pixels[i * 4u + 3u] != 255u) return false;
    }
    return true;
}

static inline uint32_t canvas_fast_rgb565(uint32_t pixel)
{
#if defined(_MIPS_ARCH_ALLEGREX)
    uint32_t red, green, blue;
    __asm__("ext %0,%1,3,5" : "=r" (red) : "r" (pixel));
    __asm__("ext %0,%1,10,6" : "=r" (green) : "r" (pixel));
    __asm__("ext %0,%1,19,5" : "=r" (blue) : "r" (pixel));
    __asm__("ins %0,%1,5,6" : "+r" (red) : "r" (green));
    __asm__("ins %0,%1,11,5" : "+r" (red) : "r" (blue));
    return red;
#else
    return ((pixel & UINT32_C(0x000000f8)) << 8)
        | ((pixel & UINT32_C(0x0000fc00)) >> 5)
        | ((pixel & UINT32_C(0x00f80000)) >> 19);
#endif
}

static inline uint32_t canvas_fast_rgb565_pair(uint32_t low, uint32_t high)
{
#if defined(_MIPS_ARCH_ALLEGREX)
    __asm__("ins %0,%1,16,16" : "+r" (low) : "r" (high));
    return low;
#else
    return low | (high << 16);
#endif
}

/* Exact nearest-neighbour 2:3 expansion used by a 320-pixel WebGL drawing
   buffer presented across the 480-pixel PSP viewport. Four source pixels
   become p0,p0,p1,p2,p2,p3, written as three aligned 32-bit pairs. */
static void canvas_fast_scale_row_3_to_2(
    uint16_t *output, const unsigned char *source, int source_width)
{
    const uint32_t *input = (const uint32_t *) (const void *) source;
    uint32_t *pairs = (uint32_t *) (void *) output;
    for (int x = 0; x < source_width; x += 4) {
        uint32_t p0 = canvas_fast_rgb565(input[x]);
        uint32_t p1 = canvas_fast_rgb565(input[x + 1]);
        uint32_t p2 = canvas_fast_rgb565(input[x + 2]);
        uint32_t p3 = canvas_fast_rgb565(input[x + 3]);
        *pairs++ = canvas_fast_rgb565_pair(p0, p0);
        *pairs++ = canvas_fast_rgb565_pair(p1, p2);
        *pairs++ = canvas_fast_rgb565_pair(p2, p3);
    }
}

static bool canvas_fast_frame_candidate_impl(
    const TileCache *cache, int scroll_y, int viewport_width,
    int viewport_height, bool require_pending,
    CanvasFastFrameCandidate *candidate)
{
    if (candidate != NULL) memset(candidate, 0, sizeof(*candidate));
    if (cache == NULL || candidate == NULL
        || (require_pending && !cache->canvas_paint_pending)
        || cache->frame == NULL || cache->layout == NULL
        || cache->frame_work.pending || cache->frame_work.ready
        || !cache->last_frame_scroll_valid
        || cache->last_frame_scroll_y != scroll_y
        || viewport_width <= 0 || viewport_height <= 0
        || cache->frame_pixels
               < (size_t) viewport_width * (size_t) viewport_height
        || cache->layout->paint_order_count != cache->layout->count
        || layout_has_css_backdrop_filter(cache->layout)) return false;

    size_t canvas_count = 0;
    for (size_t order = 0; order < cache->layout->count; order++) {
        size_t index = cache->layout->paint_order[order];
        if (index >= cache->layout->count) return false;
        const DrawCommand *command = &cache->layout->commands[index];
        if (command->type != DRAW_IMAGE || command->image == NULL
            || !command->image->is_canvas) continue;
        canvas_count++;
        bool positioned_range = false;
        for (size_t range = 0; range < cache->layout->fixed_count; range++) {
            const FixedRange *fixed = &cache->layout->fixed_ranges[range];
            if (index >= fixed->command_start && index < fixed->command_end) {
                positioned_range = true;
                break;
            }
        }
        for (size_t range = 0;
             !positioned_range && range < cache->layout->sticky_count;
             range++) {
            const StickyRange *sticky = &cache->layout->sticky_ranges[range];
            if (index >= sticky->command_start
                && index < sticky->command_end) positioned_range = true;
        }
        uint8_t flags = cache->layout->command_flags == NULL
            ? 0 : cache->layout->command_flags[index];
        if (canvas_count != 1u
            || positioned_range
            || (flags & (LAYOUT_COMMAND_FIXED
                         | LAYOUT_COMMAND_LATE_POSITIONED)) != 0
            || (flags != 0 && (flags & LAYOUT_COMMAND_OVERFLOW) == 0)
            || command->image_fit != LAYOUT_IMAGE_FIT_STRETCH
            || draw_command_rotation_quadrants(command) != 0u
            || draw_command_image_clip_radius(command) != 0
            || draw_command_filter_code(command) != 0u
            || draw_command_blend_mode(command) != LAYOUT_MIX_BLEND_NORMAL
            || command->opacity_scale != 256u) {
            return false;
        }
        candidate->command = *command;
        candidate->image = command->image;
        candidate->command_index = index;
        candidate->paint_order = order;
        candidate->clip_left = 0;
        candidate->clip_top = scroll_y;
        candidate->clip_right = viewport_width;
        candidate->clip_bottom = scroll_y + viewport_height;
        if ((flags & LAYOUT_COMMAND_OVERFLOW) != 0
            || (cache->layout->command_flags == NULL
                && command_in_overflow(cache->layout, index))) {
            int dx = 0, dy = 0;
            const OverflowGeometry *geometry =
                overflow_cached_geometry(cache, index);
            if (geometry != NULL) {
                if (!geometry->found) return false;
                dx = geometry->dx;
                dy = geometry->dy;
                candidate->clip_left = geometry->clip_left;
                candidate->clip_top = geometry->clip_top;
                candidate->clip_right = geometry->clip_right;
                candidate->clip_bottom = geometry->clip_bottom;
                RoundedClip rounded[MAXIMUM_ROUNDED_CLIPS];
                if (overflow_cached_rounded_clips(
                        cache, index, rounded,
                        MAXIMUM_ROUNDED_CLIPS) != 0) return false;
            } else {
                if (!overflow_command_geometry(
                        cache->layout, index, &dx, &dy,
                        &candidate->clip_left, &candidate->clip_top,
                        &candidate->clip_right, &candidate->clip_bottom)) {
                    return false;
                }
                RoundedClip rounded[MAXIMUM_ROUNDED_CLIPS];
                if (overflow_rounded_clips(
                        cache->layout, index, rounded,
                        MAXIMUM_ROUNDED_CLIPS) != 0) return false;
            }
            candidate->command.x = render_saturating_add_int(
                candidate->command.x, dx);
            candidate->command.y = render_saturating_add_int(
                candidate->command.y, dy);
        }
        if (candidate->clip_left < 0) candidate->clip_left = 0;
        if (candidate->clip_top < scroll_y) candidate->clip_top = scroll_y;
        if (candidate->clip_right > viewport_width)
            candidate->clip_right = viewport_width;
        if (candidate->clip_bottom > scroll_y + viewport_height)
            candidate->clip_bottom = scroll_y + viewport_height;
        if (candidate->clip_right <= candidate->clip_left
            || candidate->clip_bottom <= candidate->clip_top
            || !intersects(&candidate->command,
                           candidate->clip_left, candidate->clip_top,
                           candidate->clip_right, candidate->clip_bottom)) {
            return false;
        }
    }
    return canvas_count == 1u
        && (candidate->image->canvas_opaque
            || canvas_surface_is_opaque(candidate->image));
}

static bool canvas_fast_frame_candidate(
    const TileCache *cache, int scroll_y, int viewport_width,
    int viewport_height, CanvasFastFrameCandidate *candidate)
{
    return canvas_fast_frame_candidate_impl(
        cache, scroll_y, viewport_width, viewport_height, true, candidate);
}

/* Animated canvases are often followed by a small HTML HUD. Retain only
   disjoint post-canvas regions: a top score and bottom power label should not
   turn into a second viewport-sized surface. On a DOM mutation the exact
   damage region is cleared and rerasterized in place, so the common fixed-
   width score update never rebuilds the other HUD regions. */
enum { CANVAS_OVERLAY_PIXEL_LIMIT = 32768 };

typedef struct {
    RenderCanvasOverlayRegion regions[TILEFINCH_CANVAS_OVERLAY_REGION_LIMIT];
    size_t count;
    size_t pixels;
} CanvasOverlayPlan;

typedef struct {
    DrawCommand command;
    int clip_left;
    int clip_top;
    int clip_right;
    int clip_bottom;
    RoundedClip rounded_clips[MAXIMUM_ROUNDED_CLIPS];
    size_t rounded_clip_count;
    bool visible;
} CanvasOverlayCommand;

static void canvas_overlay_cache_clear(TileCache *cache)
{
    if (cache == NULL) return;
    if (cache->budget != NULL) {
        budget_free(cache->budget, cache->canvas_overlay_alpha);
        budget_free(cache->budget, cache->canvas_overlay_pixels);
    }
    cache->canvas_overlay_pixels = NULL;
    cache->canvas_overlay_alpha = NULL;
    cache->canvas_overlay_region_count = 0;
    cache->canvas_overlay_pixel_count = 0;
    cache->canvas_overlay_ready = false;
}

static bool canvas_overlay_plan_add(CanvasOverlayPlan *plan,
                                    int left, int top, int right, int bottom)
{
    RenderCanvasOverlayRegion next = {
        .left = (int16_t) left, .top = (int16_t) top,
        .width = (uint16_t) (right - left),
        .height = (uint16_t) (bottom - top)
    };
    for (size_t i = 0; i < plan->count;) {
        RenderCanvasOverlayRegion *region = &plan->regions[i];
        if (next.left >= region->left + (int) region->width
            || next.left + (int) next.width <= region->left
            || next.top >= region->top + (int) region->height
            || next.top + (int) next.height <= region->top) {
            i++;
            continue;
        }
        int merged_left = next.left < region->left
            ? next.left : region->left;
        int merged_top = next.top < region->top ? next.top : region->top;
        int merged_right = next.left + (int) next.width;
        int region_right = region->left + (int) region->width;
        if (region_right > merged_right) merged_right = region_right;
        int merged_bottom = next.top + (int) next.height;
        int region_bottom = region->top + (int) region->height;
        if (region_bottom > merged_bottom) merged_bottom = region_bottom;
        next.left = (int16_t) merged_left;
        next.top = (int16_t) merged_top;
        next.width = (uint16_t) (merged_right - merged_left);
        next.height = (uint16_t) (merged_bottom - merged_top);
        plan->count--;
        memmove(&plan->regions[i], &plan->regions[i + 1u],
                (plan->count - i) * sizeof(*plan->regions));
        i = 0;
    }
    if (plan->count >= TILEFINCH_CANVAS_OVERLAY_REGION_LIMIT) return false;
    plan->regions[plan->count++] = next;
    return true;
}

static void canvas_overlay_command(
    TileCache *cache, size_t index, int scroll_y,
    int viewport_width, int viewport_height, CanvasOverlayCommand *resolved)
{
    memset(resolved, 0, sizeof(*resolved));
    resolved->command = cache->layout->commands[index];
    resolved->clip_left = 0;
    resolved->clip_top = scroll_y;
    resolved->clip_right = viewport_width;
    resolved->clip_bottom = scroll_y + viewport_height;
    uint8_t flags = cache->layout->command_flags[index];
    bool overflow = (flags & LAYOUT_COMMAND_OVERFLOW) != 0;
    if (overflow) {
        int dx = 0, dy = 0;
        if (!overflow_command_geometry(
                cache->layout, index, &dx, &dy,
                &resolved->clip_left, &resolved->clip_top,
                &resolved->clip_right, &resolved->clip_bottom)) return;
        resolved->command.x = render_saturating_add_int(
            resolved->command.x, dx);
        resolved->command.y = render_saturating_add_int(
            resolved->command.y, dy);
        resolved->rounded_clip_count = overflow_rounded_clips(
            cache->layout, index, resolved->rounded_clips,
            MAXIMUM_ROUNDED_CLIPS);
    }
    if (resolved->clip_left < 0) resolved->clip_left = 0;
    if (resolved->clip_top < scroll_y) resolved->clip_top = scroll_y;
    if (resolved->clip_right > viewport_width) {
        resolved->clip_right = viewport_width;
    }
    if (resolved->clip_bottom > scroll_y + viewport_height) {
        resolved->clip_bottom = scroll_y + viewport_height;
    }
    resolved->visible = resolved->clip_right > resolved->clip_left
        && resolved->clip_bottom > resolved->clip_top
        && intersects(&resolved->command, resolved->clip_left,
                      resolved->clip_top, resolved->clip_right,
                      resolved->clip_bottom);
}

static bool canvas_overlay_plan(
    TileCache *cache, const CanvasFastFrameCandidate *candidate,
    int scroll_y, int viewport_width, int viewport_height,
    CanvasOverlayPlan *plan)
{
    memset(plan, 0, sizeof(*plan));
    if (cache->layout->sticky_count != 0
        || cache->layout->command_flags == NULL) return false;
    for (size_t order = candidate->paint_order + 1u;
         order < cache->layout->count; order++) {
        size_t index = cache->layout->paint_order[order];
        uint8_t flags = cache->layout->command_flags[index];
        if ((flags & LAYOUT_COMMAND_FIXED) != 0) continue;
        bool overflow = (flags & LAYOUT_COMMAND_OVERFLOW) != 0;
        if (!overflow && flags != 0
            && (flags & LAYOUT_COMMAND_LATE_POSITIONED) == 0) {
            continue;
        }
        const DrawCommand *command = &cache->layout->commands[index];
        if (draw_command_blend_mode(command) != LAYOUT_MIX_BLEND_NORMAL
            || draw_command_backdrop_blur(command) != 0u
            || (command->type == DRAW_IMAGE && command->image != NULL
                && command->image->is_canvas)) return false;
        CanvasOverlayCommand resolved;
        canvas_overlay_command(
            cache, index, scroll_y, viewport_width, viewport_height,
            &resolved);
        if (!resolved.visible) continue;
        int left = resolved.command.x > resolved.clip_left
            ? resolved.command.x : resolved.clip_left;
        int top = resolved.command.y > resolved.clip_top
            ? resolved.command.y : resolved.clip_top;
        int right = resolved.command.x + resolved.command.width;
        int bottom = resolved.command.y + resolved.command.height;
        if (right > resolved.clip_right) right = resolved.clip_right;
        if (bottom > resolved.clip_bottom) bottom = resolved.clip_bottom;
        if (right <= left || bottom <= top) continue;
        if (!canvas_overlay_plan_add(
                plan, left, top - scroll_y, right, bottom - scroll_y)) {
            return false;
        }
    }
    for (size_t i = 0; i < plan->count; i++) {
        RenderCanvasOverlayRegion *region = &plan->regions[i];
        size_t area = (size_t) region->width * region->height;
        if (area > CANVAS_OVERLAY_PIXEL_LIMIT - plan->pixels) return false;
        region->pixel_offset = (uint32_t) plan->pixels;
        plan->pixels += area;
    }
    return true;
}

static void canvas_overlay_raster(
    TileCache *cache, const CanvasFastFrameCandidate *candidate,
    uint8_t region_mask)
{
    for (size_t order = candidate->paint_order + 1u;
         order < cache->layout->count; order++) {
        size_t index = cache->layout->paint_order[order];
        uint8_t flags = cache->layout->command_flags[index];
        bool overflow = (flags & LAYOUT_COMMAND_OVERFLOW) != 0;
        if ((flags & LAYOUT_COMMAND_FIXED) != 0
            || (!overflow && flags != 0
                && (flags & LAYOUT_COMMAND_LATE_POSITIONED) == 0)) continue;
        CanvasOverlayCommand resolved;
        canvas_overlay_command(
            cache, index, cache->canvas_overlay_scroll_y,
            cache->canvas_overlay_viewport_width,
            cache->canvas_overlay_viewport_height, &resolved);
        if (!resolved.visible) continue;
        const DrawCommand *command = &resolved.command;
        for (size_t i = 0; i < cache->canvas_overlay_region_count; i++) {
            if ((region_mask & (UINT8_C(1) << i)) == 0) continue;
            const RenderCanvasOverlayRegion *region =
                &cache->canvas_overlay_regions[i];
            int world_top = cache->canvas_overlay_scroll_y + region->top;
            if (!intersects(command, region->left, world_top,
                            region->left + region->width,
                            world_top + region->height)) continue;
            RasterTarget target = {
                .pixels = cache->canvas_overlay_pixels
                    + region->pixel_offset,
                .alpha = cache->canvas_overlay_alpha
                    + region->pixel_offset,
                .stride = region->width,
                .origin_x = region->left,
                .origin_y = world_top,
                .width = region->width,
                .height = region->height,
                .left = region->left,
                .top = world_top,
                .right = region->left + region->width,
                .bottom = world_top + region->height,
                .rounded_clips = resolved.rounded_clips,
                .rounded_clip_count = resolved.rounded_clip_count
            };
            if (resolved.clip_left > target.left) {
                target.left = resolved.clip_left;
            }
            if (resolved.clip_top > target.top) target.top = resolved.clip_top;
            if (resolved.clip_right < target.right) {
                target.right = resolved.clip_right;
            }
            if (resolved.clip_bottom < target.bottom) {
                target.bottom = resolved.clip_bottom;
            }
            rasterize_command(cache, &target, command);
        }
    }
}

static bool canvas_overlay_build(
    TileCache *cache, const CanvasFastFrameCandidate *candidate,
    int scroll_y, int viewport_width, int viewport_height)
{
    CanvasOverlayPlan plan;
    if (!canvas_overlay_plan(cache, candidate, scroll_y, viewport_width,
                             viewport_height, &plan)) return false;
    canvas_overlay_cache_clear(cache);
    if (plan.pixels != 0) {
        cache->canvas_overlay_pixels = budget_calloc(
            cache->budget, plan.pixels,
            sizeof(*cache->canvas_overlay_pixels));
        cache->canvas_overlay_alpha = budget_calloc(
            cache->budget, plan.pixels,
            sizeof(*cache->canvas_overlay_alpha));
        if (cache->canvas_overlay_pixels == NULL
            || cache->canvas_overlay_alpha == NULL) {
            canvas_overlay_cache_clear(cache);
            return false;
        }
    }
    memcpy(cache->canvas_overlay_regions, plan.regions,
           plan.count * sizeof(*plan.regions));
    cache->canvas_overlay_region_count = plan.count;
    cache->canvas_overlay_pixel_count = plan.pixels;
    cache->canvas_overlay_scroll_y = scroll_y;
    cache->canvas_overlay_viewport_width = viewport_width;
    cache->canvas_overlay_viewport_height = viewport_height;
    if (plan.count != 0) {
        uint8_t all = (uint8_t) ((UINT16_C(1) << plan.count) - 1u);
        canvas_overlay_raster(cache, candidate, all);
    }
    cache->canvas_overlay_ready = true;
    cache->canvas_overlay_builds++;
    return true;
}

static bool canvas_overlay_patch(
    TileCache *cache, const CanvasFastFrameCandidate *candidate,
    int damage_left, int damage_top, int damage_right, int damage_bottom)
{
    CanvasOverlayPlan plan;
    if (!canvas_overlay_plan(
            cache, candidate, cache->canvas_overlay_scroll_y,
            cache->canvas_overlay_viewport_width,
            cache->canvas_overlay_viewport_height, &plan)
        || !cache->canvas_overlay_ready
        || cache->canvas_overlay_region_count != plan.count
        || cache->canvas_overlay_pixel_count != plan.pixels) return false;
    for (size_t i = 0; i < plan.count; i++) {
        const RenderCanvasOverlayRegion *left =
            &cache->canvas_overlay_regions[i];
        const RenderCanvasOverlayRegion *right = &plan.regions[i];
        if (left->left != right->left || left->top != right->top
            || left->width != right->width
            || left->height != right->height) return false;
    }
    uint8_t affected = 0;
    int screen_top = damage_top - cache->canvas_overlay_scroll_y;
    int screen_bottom = damage_bottom - cache->canvas_overlay_scroll_y;
    for (size_t i = 0; i < cache->canvas_overlay_region_count; i++) {
        const RenderCanvasOverlayRegion *region =
            &cache->canvas_overlay_regions[i];
        if (region->left < damage_right
            && region->left + (int) region->width > damage_left
            && region->top < screen_bottom
            && region->top + (int) region->height > screen_top) {
            affected |= (uint8_t) (UINT8_C(1) << i);
        }
    }
    if (affected == 0) return true;
    for (size_t i = 0; i < cache->canvas_overlay_region_count; i++) {
        if ((affected & (UINT8_C(1) << i)) == 0) continue;
        const RenderCanvasOverlayRegion *region =
            &cache->canvas_overlay_regions[i];
        size_t pixels = (size_t) region->width * region->height;
        memset(cache->canvas_overlay_pixels + region->pixel_offset, 0,
               pixels * sizeof(*cache->canvas_overlay_pixels));
        memset(cache->canvas_overlay_alpha + region->pixel_offset, 0,
               pixels * sizeof(*cache->canvas_overlay_alpha));
        cache->canvas_overlay_patch_regions++;
    }
    canvas_overlay_raster(cache, candidate, affected);
    cache->canvas_overlay_patches++;
    return true;
}

static void canvas_overlay_blit(TileCache *cache, uint16_t *frame,
                                int viewport_width, int viewport_height)
{
    for (size_t i = 0; i < cache->canvas_overlay_region_count; i++) {
        const RenderCanvasOverlayRegion *region =
            &cache->canvas_overlay_regions[i];
        for (size_t y = 0; y < region->height; y++) {
            int screen_y = region->top + (int) y;
            if (screen_y < 0 || screen_y >= viewport_height) continue;
            for (uint16_t x = 0; x < region->width; x++) {
                int screen_x = region->left + x;
                if (screen_x < 0 || screen_x >= viewport_width) continue;
                size_t source = region->pixel_offset
                    + y * region->width + x;
                unsigned alpha = cache->canvas_overlay_alpha[source];
                if (alpha == 0) continue;
                size_t destination = (size_t) screen_y * viewport_width
                    + (size_t) screen_x;
                if (alpha == 255) {
                    frame[destination] = cache->canvas_overlay_pixels[source];
                } else {
                    uint16_t color = cache->canvas_overlay_pixels[source];
                    uint32_t rgb =
                        (tilefinch_rgb5_to_u8(
                             tilefinch_rgb565_red_code(color)) << 16)
                        | (tilefinch_rgb6_to_u8(
                               tilefinch_rgb565_green_code(color)) << 8)
                        | tilefinch_rgb5_to_u8(
                            tilefinch_rgb565_blue_code(color));
                    frame[destination] = blend_rgb565(
                        frame[destination], rgb, alpha);
                }
            }
        }
    }
}

static void rasterize_opaque_canvas_fast(
    RasterTarget *target, const DrawCommand *command,
    const ImageResource *image)
{
    const unsigned char *source_pixels = image->pixels;
    size_t source_stride = (size_t) image->width * 4u;
    const unsigned char *native_pixels = NULL;
    size_t native_stride = 0u;
    if (image_resource_native_canvas_source(
            image, &native_pixels, &native_stride)) {
        source_pixels = native_pixels;
        source_stride = native_stride;
    }
    int64_t right = (int64_t) command->x + command->width;
    int64_t bottom = (int64_t) command->y + command->height;
    int x0 = command->x > target->left ? command->x : target->left;
    int y0 = command->y > target->top ? command->y : target->top;
    int x1 = right < target->right ? (int) right : target->right;
    int y1 = bottom < target->bottom ? (int) bottom : target->bottom;
    ScaleStepper source_y = scale_stepper(
        y0 - command->y, image->height, command->height);
    int previous_source_y = -1;
    int previous_span_x0 = 0;
    int previous_span_x1 = 0;
    size_t previous_destination = 0;
    for (int y = y0; y < y1; y++) {
        int span_x0 = 0, span_x1 = 0;
        size_t destination = 0;
        bool rounded = false;
        if (!raster_target_span(target, y, x0, x1,
                                &span_x0, &span_x1,
                                &destination, &rounded)) {
            scale_stepper_advance(&source_y);
            continue;
        }
        size_t span_pixels = (size_t) (span_x1 - span_x0);
        if (source_y.value == previous_source_y
            && span_x0 == previous_span_x0
            && span_x1 == previous_span_x1) {
            memcpy(target->pixels + destination,
                   target->pixels + previous_destination,
                   span_pixels * sizeof(*target->pixels));
            previous_destination = destination;
            scale_stepper_advance(&source_y);
            continue;
        }
        ScaleStepper source_x = scale_stepper(
            span_x0 - command->x, image->width, command->width);
        const unsigned char *source_row = source_pixels
            + (size_t) source_y.value * source_stride;
        /* A 320-wide drawing buffer scaled across the PSP's 480 columns is
           the common game path.  Its exact nearest-neighbour mapping repeats
           each even source pixel twice and each odd source pixel once.  Use
           that fixed rational kernel instead of advancing and branching a
           ScaleStepper for every destination pixel.  Other ratios and clips
           retain the general pixel-identical path below. */
        if (span_x0 == command->x
            && span_x1 - span_x0 == command->width
            && image->width > 0
            && image->width % 2 == 0
            && image->width % 4 == 0
            && command->width > 0 && command->width % 3 == 0
            && command->width / 3 * 2 == image->width
            && ((uintptr_t) (const void *) source_row % 4u) == 0u
            && ((uintptr_t) (void *)
                    (target->pixels + destination) % 4u) == 0u) {
            canvas_fast_scale_row_3_to_2(
                target->pixels + destination, source_row, image->width);
            previous_source_y = source_y.value;
            previous_span_x0 = span_x0;
            previous_span_x1 = span_x1;
            previous_destination = destination;
            scale_stepper_advance(&source_y);
            continue;
        }
        int previous_source_x = -1;
        uint16_t converted = 0;
        for (int x = span_x0; x < span_x1; x++, destination++) {
            if (source_x.value != previous_source_x) {
                const unsigned char *pixel = source_row
                    + (size_t) source_x.value * 4u;
                converted = (uint16_t) (
                    ((uint16_t) (pixel[0] & 0xf8u) << 8)
                    | ((uint16_t) (pixel[1] & 0xfcu) << 3)
                    | ((uint16_t) pixel[2] >> 3));
                previous_source_x = source_x.value;
            }
            target->pixels[destination] = converted;
            scale_stepper_advance(&source_x);
        }
        previous_source_y = source_y.value;
        previous_span_x0 = span_x0;
        previous_span_x1 = span_x1;
        previous_destination = destination - span_pixels;
        scale_stepper_advance(&source_y);
    }
}

bool tile_cache_canvas_frame_fast_eligible(
    const TileCache *cache, int scroll_y,
    int viewport_width, int viewport_height)
{
    if (cache == NULL || cache->source_layout == NULL
        || cache->layout == NULL) return false;
    scroll_y = viewport_css_to_device(&cache->source_layout->viewport,
                                      scroll_y);
    int maximum_scroll = cache->layout->height - viewport_height;
    if (maximum_scroll < 0) maximum_scroll = 0;
    if (scroll_y > maximum_scroll) scroll_y = maximum_scroll;
    CanvasFastFrameCandidate candidate;
    return canvas_fast_frame_candidate(
        cache, scroll_y, viewport_width, viewport_height, &candidate);
}

RenderCanvasFrameResult tile_cache_render_canvas_frame_fast(
    TileCache *cache, int scroll_y, int viewport_width, int viewport_height)
{
    if (cache == NULL || cache->source_layout == NULL) {
        return RENDER_CANVAS_FRAME_FAILED;
    }
    scroll_y = viewport_css_to_device(&cache->source_layout->viewport,
                                      scroll_y);
    int maximum_scroll = cache->layout->height - viewport_height;
    if (maximum_scroll < 0) maximum_scroll = 0;
    if (scroll_y > maximum_scroll) scroll_y = maximum_scroll;
    CanvasFastFrameCandidate candidate;
    bool canvas_pending = cache->canvas_paint_pending;
    if (!canvas_fast_frame_candidate(
            cache, scroll_y, viewport_width, viewport_height, &candidate)) {
        if (canvas_pending) cache->canvas_fast_refusals++;
        return RENDER_CANVAS_FRAME_NOT_APPLICABLE;
    }

    uint64_t started = render_now_us();
    RasterTarget target = {
        .pixels = cache->frame,
        .stride = (size_t) viewport_width,
        .origin_x = 0,
        .origin_y = scroll_y,
        .width = viewport_width,
        .height = viewport_height,
        .left = candidate.clip_left,
        .top = candidate.clip_top,
        .right = candidate.clip_right,
        .bottom = candidate.clip_bottom
    };
    rasterize_opaque_canvas_fast(
        &target, &candidate.command, candidate.image);
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    uint64_t rastered = render_now_us();
#endif
    bool overlay_cached = cache->canvas_overlay_ready
        && cache->canvas_overlay_scroll_y == scroll_y
        && cache->canvas_overlay_viewport_width == viewport_width
        && cache->canvas_overlay_viewport_height == viewport_height;
    if (!overlay_cached) {
        overlay_cached = canvas_overlay_build(
            cache, &candidate, scroll_y, viewport_width, viewport_height);
    }
    if (overlay_cached) {
        canvas_overlay_blit(cache, cache->frame,
                            viewport_width, viewport_height);
    } else {
        target.left = 0;
        target.top = scroll_y;
        target.right = viewport_width;
        target.bottom = scroll_y + viewport_height;
        for (size_t order = candidate.paint_order + 1u;
             order < cache->layout->count; order++) {
            size_t index = cache->layout->paint_order[order];
            uint8_t flags = cache->layout->command_flags == NULL
                ? 0 : cache->layout->command_flags[index];
            if ((flags & LAYOUT_COMMAND_FIXED) != 0) continue;
            if ((flags & LAYOUT_COMMAND_OVERFLOW) != 0
                || (cache->layout->command_flags == NULL
                    && command_in_overflow(cache->layout, index))) {
                paint_overflow_command(cache, cache->frame, index, scroll_y,
                                       viewport_width, viewport_height);
                continue;
            }
            if ((flags & LAYOUT_COMMAND_LATE_POSITIONED) != 0) {
                paint_overlay_command(
                    cache, cache->frame, &cache->layout->commands[index],
                    scroll_y, viewport_width, viewport_height);
                continue;
            }
            if (flags != 0) continue;
            cache->command_candidates++;
            rasterize_command(
                cache, &target, &cache->layout->commands[index]);
        }
    }
    paint_sticky_overlays(cache, cache->frame, scroll_y,
                          viewport_width, viewport_height);
    if (cache->layout->fixed_count != 0) {
        if (build_fixed_cache(cache, viewport_width, viewport_height)) {
            paint_cached_fixed_overlays(cache, cache->frame,
                                        viewport_width, viewport_height);
            paint_fixed_overlays(cache, cache->frame, scroll_y,
                                 viewport_width, viewport_height, true);
        } else {
            paint_fixed_overlays(cache, cache->frame, scroll_y,
                                 viewport_width, viewport_height, false);
        }
    }
    paint_scroll_indicator(cache, cache->frame, scroll_y,
                           viewport_width, viewport_height);
    cache->last_frame_scroll_y = scroll_y;
    cache->last_frame_scroll_valid = true;
    cache->canvas_paint_pending = false;
    memset(&cache->frame_work, 0, sizeof(cache->frame_work));
    uint64_t elapsed = render_now_us() - started;
    cache->canvas_fast_frames++;
    cache->canvas_fast_us += elapsed;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    cache->canvas_fast_raster_us += rastered - started;
    cache->canvas_fast_overlay_us += elapsed - (rastered - started);
#endif
    if (elapsed > cache->canvas_fast_max_us) {
        cache->canvas_fast_max_us = elapsed;
    }
    cache->frame_us += elapsed;
    if (elapsed > cache->max_frame_us) cache->max_frame_us = elapsed;
    cache->frames_rendered++;
    return RENDER_CANVAS_FRAME_COMPLETE;
}

static void copy_tile_to_frame(const RenderTile *tile, uint16_t *frame,
                               int scroll_y, int viewport_width,
                               int viewport_height)
{
    int tile_left = tile->tile_x * TILEFINCH_TILE_SIZE;
    int tile_top = tile->tile_y * TILEFINCH_TILE_SIZE;
    int x0 = tile_left < 0 ? 0 : tile_left;
    int y0 = tile_top < scroll_y ? scroll_y : tile_top;
    int x1 = tile_left + TILEFINCH_TILE_SIZE < viewport_width
             ? tile_left + TILEFINCH_TILE_SIZE : viewport_width;
    int y1 = tile_top + TILEFINCH_TILE_SIZE < scroll_y + viewport_height
             ? tile_top + TILEFINCH_TILE_SIZE : scroll_y + viewport_height;
    for (int world_y = y0; world_y < y1; world_y++) {
        int screen_y = world_y - scroll_y;
        int local_y = world_y - tile_top;
        int local_x = x0 - tile_left;
        memcpy(frame + (size_t) screen_y * viewport_width + x0,
               tile->pixels + (size_t) local_y * TILEFINCH_TILE_SIZE + local_x,
               (size_t) (x1 - x0) * sizeof(*frame));
    }
}

void tile_cache_cancel_frame_work(TileCache *cache)
{
    if (cache == NULL) return;
    if (cache->frame_work.pending) cache->frame_jobs_cancelled++;
    memset(&cache->frame_work, 0, sizeof(cache->frame_work));
    cache->canvas_paint_pending = false;
}

bool tile_cache_frame_work_pending(const TileCache *cache)
{
    return cache != NULL && cache->frame_work.pending;
}

bool tile_cache_canvas_frame_work_pending(const TileCache *cache)
{
    return cache != NULL
        && (cache->canvas_paint_pending
            || ((cache->frame_work.pending || cache->frame_work.ready)
                && cache->frame_work.canvas_paint));
}

static bool frame_work_matches(const RenderFrameWork *work, int scroll_y,
                               int viewport_width, int viewport_height)
{
    return work != NULL && (work->pending || work->ready)
        && work->scroll_y == scroll_y
        && work->viewport_width == viewport_width
        && work->viewport_height == viewport_height;
}

static void tile_cache_schedule_frame_work(TileCache *cache, int scroll_y,
                                           int viewport_width,
                                           int viewport_height)
{
    RenderFrameWork *work = &cache->frame_work;
    if (frame_work_matches(work, scroll_y, viewport_width, viewport_height)) {
        return;
    }
    bool canvas_paint = cache->canvas_paint_pending;
    tile_cache_cancel_frame_work(cache);
    tile_cache_sync_visual_scroll(cache);
    int first_tx = 0;
    int last_tx = (viewport_width - 1) / TILEFINCH_TILE_SIZE;
    int first_ty = scroll_y / TILEFINCH_TILE_SIZE;
    int last_ty = (scroll_y + viewport_height - 1) / TILEFINCH_TILE_SIZE;
    size_t columns = (size_t) (last_tx - first_tx + 1);
    size_t rows = (size_t) (last_ty - first_ty + 1);
    size_t required = columns * rows;
    size_t selected = required < cache->tile_capacity
        ? required : cache->tile_capacity;
    size_t first = 0;
    if (required > selected
        && (!cache->last_frame_scroll_valid
            || scroll_y >= cache->last_frame_scroll_y)) {
        first = required - selected;
    }
    *work = (RenderFrameWork) {
        .scroll_y = scroll_y,
        .viewport_width = viewport_width,
        .viewport_height = viewport_height,
        .first_tx = first_tx,
        .first_ty = first_ty,
        .columns = columns,
        .required_tiles = required,
        .next_ordinal = first,
        .end_ordinal = first + selected,
        .pending = selected != 0,
        .ready = selected == 0,
        .canvas_paint = canvas_paint
    };
    cache->canvas_paint_pending = false;
    cache->frame_jobs_scheduled++;
    if (work->ready) cache->frame_jobs_completed++;
}

RenderFrameWorkResult tile_cache_prepare_frame_bounded(
    TileCache *cache, int scroll_y, int viewport_width, int viewport_height,
    uint64_t budget_us, size_t maximum_units)
{
    return tile_cache_prepare_frame_bounded_cancelable(
        cache, scroll_y, viewport_width, viewport_height,
        budget_us, maximum_units, NULL);
}

RenderFrameWorkResult tile_cache_prepare_frame_bounded_cancelable(
    TileCache *cache, int scroll_y, int viewport_width, int viewport_height,
    uint64_t budget_us, size_t maximum_units,
    const TilefinchCancellation *cancellation)
{
    if (cache == NULL || cache->tiles == NULL || viewport_width <= 0
        || viewport_height <= 0 || scroll_y < 0) {
        return RENDER_FRAME_WORK_FAILED;
    }
    if (tilefinch_cancellation_requested(cancellation)) {
        tile_cache_cancel_frame_work(cache);
        return RENDER_FRAME_WORK_CANCELLED;
    }
    scroll_y = viewport_css_to_device(
        &cache->source_layout->viewport, scroll_y);
    int maximum_scroll = cache->layout->height - viewport_height;
    if (maximum_scroll < 0) maximum_scroll = 0;
    if (scroll_y > maximum_scroll) scroll_y = maximum_scroll;
    tile_cache_schedule_frame_work(
        cache, scroll_y, viewport_width, viewport_height);
    RenderFrameWork *work = &cache->frame_work;
    if (work->required_tiles > cache->tile_capacity
        && cache->frame_scratch_tile == NULL) {
        cache->frame_scratch_tile = budget_malloc(
            cache->budget, sizeof(*cache->frame_scratch_tile));
        if (cache->frame_scratch_tile == NULL) {
            tile_cache_cancel_frame_work(cache);
            return RENDER_FRAME_WORK_FAILED;
        }
    }
    if (work->ready) return RENDER_FRAME_WORK_READY;
    if (maximum_units == 0 || budget_us == 0) {
        return RENDER_FRAME_WORK_PENDING;
    }

    uint64_t started = render_now_us();
    size_t units = 0;
    cache->frame_job_slices++;
    while (work->next_ordinal < work->end_ordinal
           && units < maximum_units) {
        if (tilefinch_cancellation_requested(cancellation)) {
            tile_cache_cancel_frame_work(cache);
            return RENDER_FRAME_WORK_CANCELLED;
        }
        if (units != 0 && render_now_us() - started >= budget_us) break;
        size_t ordinal = work->next_ordinal++;
        int tx = work->first_tx + (int) (ordinal % work->columns);
        int ty = work->first_ty + (int) (ordinal / work->columns);
        uint64_t unit_started = render_now_us();
        if (ensure_tile(cache, tx, ty) == NULL) {
            tile_cache_cancel_frame_work(cache);
            return tilefinch_cancellation_requested(cancellation)
                ? RENDER_FRAME_WORK_CANCELLED
                : RENDER_FRAME_WORK_FAILED;
        }
        uint64_t unit_elapsed = render_now_us() - unit_started;
        if (unit_elapsed > cache->max_frame_job_unit_us) {
            cache->max_frame_job_unit_us = unit_elapsed;
        }
        units++;
        cache->frame_job_units++;
        if (tilefinch_cancellation_requested(cancellation)) {
            tile_cache_cancel_frame_work(cache);
            return RENDER_FRAME_WORK_CANCELLED;
        }
    }
    uint64_t elapsed = render_now_us() - started;
    cache->frame_job_us += elapsed;
    if (elapsed > cache->max_frame_job_slice_us) {
        cache->max_frame_job_slice_us = elapsed;
    }
    if (elapsed > budget_us) cache->frame_job_slice_overruns++;
    if (work->next_ordinal == work->end_ordinal) {
        work->pending = false;
        work->ready = true;
        cache->frame_jobs_completed++;
        return RENDER_FRAME_WORK_READY;
    }
    if (elapsed >= budget_us) cache->frame_job_budget_exhaustions++;
    return RENDER_FRAME_WORK_PENDING;
}

bool tile_cache_render_frame(TileCache *cache, int scroll_y,
                             int viewport_width, int viewport_height,
                             const char *output_path)
{
    if (cache == NULL || cache->tiles == NULL || viewport_width <= 0
        || viewport_height <= 0 || scroll_y < 0) return false;
    if (cache->frame_work.pending) tile_cache_cancel_frame_work(cache);
    cache->frame_work.ready = false;
    uint64_t started = render_now_us();
    tile_cache_sync_visual_scroll(cache);
    scroll_y = viewport_css_to_device(&cache->source_layout->viewport,
                                      scroll_y);
    int maximum_scroll = cache->layout->height - viewport_height;
    if (maximum_scroll < 0) maximum_scroll = 0;
    if (scroll_y > maximum_scroll) scroll_y = maximum_scroll;
    size_t pixels = (size_t) viewport_width * (size_t) viewport_height;
    bool temporary_frame = cache->frame == NULL;
    uint16_t *frame = temporary_frame
                      ? budget_malloc(cache->budget, pixels * sizeof(*frame))
                      : cache->frame;
    if (!temporary_frame && cache->frame_pixels < pixels) return false;
    if (frame == NULL) return false;
    /*
     * Prepare overflow geometry before any optional frame scratch allocation.
     * Besides making the cache available for the overlay pass, this keeps an
     * allocation failure in the geometry accelerator local to that optional
     * accelerator: rendering can then take its exact scan fallback while the
     * independent tile admission path remains free to allocate its scratch.
     */
    if (cache->layout->command_flags != NULL) {
        (void) overflow_cache_prepare(cache);
    }
    uint64_t phase_started = render_now_us();
    uint64_t phase_elapsed = phase_started - started;
    cache->frame_setup_us += phase_elapsed;
    if (phase_elapsed > cache->max_frame_setup_us) {
        cache->max_frame_setup_us = phase_elapsed;
    }
    bool ok = true;
    int first_tx = 0;
    int last_tx = (viewport_width - 1) / TILEFINCH_TILE_SIZE;
    int first_ty = scroll_y / TILEFINCH_TILE_SIZE;
    int last_ty = (scroll_y + viewport_height - 1) / TILEFINCH_TILE_SIZE;
    size_t columns = (size_t) (last_tx - first_tx + 1);
    size_t rows = (size_t) (last_ty - first_ty + 1);
    size_t required_tiles = columns * rows;
    if (required_tiles > cache->tile_capacity && required_tiles <= 64u) {
        uint64_t resident = 0;
        size_t ordinal = 0;
        for (int ty = first_ty; ty <= last_ty; ty++) {
            for (int tx = first_tx; tx <= last_tx; tx++, ordinal++) {
                RenderTile *tile = find_tile(cache, tx, ty);
                if (tile == NULL) {
                    cache->misses++;
                    continue;
                }
                resident |= UINT64_C(1) << ordinal;
                cache->hits++;
                cache->frame_preserved_tiles++;
                tile->last_used = ++cache->clock;
                copy_tile_to_frame(tile, frame, scroll_y, viewport_width,
                                   viewport_height);
            }
        }
        if (cache->frame_scratch_tile == NULL) {
            cache->frame_scratch_tile = budget_malloc(
                cache->budget, sizeof(*cache->frame_scratch_tile));
        }
        if (cache->frame_scratch_tile != NULL) {
            uint64_t required_mask = required_tiles == 64u
                ? UINT64_MAX : (UINT64_C(1) << required_tiles) - 1u;
            uint64_t desired = 0;
            if (cache->last_frame_scroll_valid
                && scroll_y == cache->last_frame_scroll_y) {
                desired = resident;
                size_t desired_count = 0;
                for (uint64_t bits = desired; bits != 0; bits &= bits - 1u) {
                    desired_count++;
                }
                for (size_t at = required_tiles;
                     at != 0 && desired_count < cache->tile_capacity;) {
                    at--;
                    uint64_t bit = UINT64_C(1) << at;
                    if (desired & bit) continue;
                    desired |= bit;
                    desired_count++;
                }
            } else if (cache->last_frame_scroll_valid
                       && scroll_y < cache->last_frame_scroll_y) {
                desired = (UINT64_C(1) << cache->tile_capacity) - 1u;
            } else {
                size_t first_desired = required_tiles - cache->tile_capacity;
                desired = required_mask
                    & ~((UINT64_C(1) << first_desired) - 1u);
            }
            uint64_t missing = required_mask & ~resident;
            ordinal = 0;
            for (int ty = first_ty; ty <= last_ty && ok; ty++) {
                for (int tx = first_tx; tx <= last_tx && ok;
                     tx++, ordinal++) {
                    uint64_t bit = UINT64_C(1) << ordinal;
                    if (!(missing & bit) || (desired & bit)) continue;
                    RenderTile *scratch = cache->frame_scratch_tile;
                    scratch->valid = true;
                    scratch->tile_x = tx;
                    scratch->tile_y = ty;
                    scratch->last_used = ++cache->clock;
                    rasterize_tile(cache, scratch);
                    copy_tile_to_frame(scratch, frame, scroll_y,
                                       viewport_width, viewport_height);
                }
            }
            ordinal = 0;
            for (int ty = first_ty; ty <= last_ty && ok; ty++) {
                for (int tx = first_tx; tx <= last_tx && ok;
                     tx++, ordinal++) {
                    uint64_t bit = UINT64_C(1) << ordinal;
                    if (!(missing & bit) || !(desired & bit)) continue;
                    RenderTile *victim = tile_victim_outside_set(
                        cache, desired, first_tx, first_ty, columns, rows);
                    RenderTile *tile = render_tile_miss(
                        cache, tx, ty, victim, false);
                    if (tile == NULL) { ok = false; break; }
                    copy_tile_to_frame(tile, frame, scroll_y, viewport_width,
                                       viewport_height);
                }
            }
        } else {
            RenderTile *churn = NULL;
            ordinal = 0;
            for (int ty = first_ty; ty <= last_ty && ok; ty++) {
                for (int tx = first_tx; tx <= last_tx && ok;
                     tx++, ordinal++) {
                    if ((resident & (UINT64_C(1) << ordinal)) != 0) continue;
                    RenderTile *victim = tile_victim(cache);
                    if (victim != NULL && victim->valid) {
                        if (churn == NULL) churn = victim;
                        victim = churn;
                    }
                    RenderTile *tile = render_tile_miss(
                        cache, tx, ty, victim, false);
                    if (tile == NULL) { ok = false; break; }
                    copy_tile_to_frame(tile, frame, scroll_y, viewport_width,
                                       viewport_height);
                }
            }
        }
    } else {
        for (int ty = first_ty; ty <= last_ty && ok; ty++) {
            for (int tx = first_tx; tx <= last_tx && ok; tx++) {
                RenderTile *tile = ensure_tile(cache, tx, ty);
                if (tile == NULL) { ok = false; break; }
                copy_tile_to_frame(tile, frame, scroll_y, viewport_width,
                                   viewport_height);
            }
        }
    }
    uint64_t phase_finished = render_now_us();
    phase_elapsed = phase_finished - phase_started;
    cache->frame_tile_us += phase_elapsed;
    if (phase_elapsed > cache->max_frame_tile_us) {
        cache->max_frame_tile_us = phase_elapsed;
    }
    phase_started = phase_finished;
    if (ok) paint_overflow_commands(cache, frame, scroll_y, viewport_width,
                                    viewport_height);
    phase_finished = render_now_us();
    phase_elapsed = phase_finished - phase_started;
    cache->frame_overflow_us += phase_elapsed;
    if (phase_elapsed > cache->max_frame_overflow_us) {
        cache->max_frame_overflow_us = phase_elapsed;
    }
    phase_started = phase_finished;
    if (ok) paint_sticky_overlays(cache, frame, scroll_y, viewport_width,
                                  viewport_height);
    phase_finished = render_now_us();
    phase_elapsed = phase_finished - phase_started;
    cache->frame_sticky_us += phase_elapsed;
    if (phase_elapsed > cache->max_frame_sticky_us) {
        cache->max_frame_sticky_us = phase_elapsed;
    }
    phase_started = phase_finished;
    if (ok && cache->layout->fixed_count != 0) {
        if (build_fixed_cache(cache, viewport_width, viewport_height)) {
            paint_cached_fixed_overlays(cache, frame, viewport_width,
                                        viewport_height);
            paint_fixed_overlays(cache, frame, scroll_y, viewport_width,
                                 viewport_height, true);
        } else {
            paint_fixed_overlays(cache, frame, scroll_y, viewport_width,
                                 viewport_height, false);
        }
    }
    phase_finished = render_now_us();
    phase_elapsed = phase_finished - phase_started;
    cache->frame_fixed_us += phase_elapsed;
    if (phase_elapsed > cache->max_frame_fixed_us) {
        cache->max_frame_fixed_us = phase_elapsed;
    }
    phase_started = phase_finished;
    if (ok) paint_scroll_indicator(cache, frame, scroll_y, viewport_width,
                                   viewport_height);
    phase_finished = render_now_us();
    cache->frame_indicator_us += phase_finished - phase_started;
    if (ok) {
        cache->last_frame_scroll_y = scroll_y;
        cache->last_frame_scroll_valid = true;
    }
    if (ok && output_path != NULL) {
        uint64_t write_started = render_now_us();
        ok = render_write_frame_ppm(
            output_path, frame,
            temporary_frame ? pixels : cache->frame_pixels,
            viewport_width, viewport_height);
        cache->frame_io_us += render_now_us() - write_started;
    }
    if (temporary_frame) budget_free(cache->budget, frame);
    uint64_t elapsed = render_now_us() - started;
    cache->frame_us += elapsed;
    if (elapsed > cache->max_frame_us) cache->max_frame_us = elapsed;
    cache->frames_rendered++;
    return ok;
}

enum {
    RENDER_IDLE_SCAN_UNIT = 32,
    RENDER_IDLE_MAXIMUM_OVERLAY_CANDIDATES = 512,
    RENDER_IDLE_MAXIMUM_OVERFLOW_CANDIDATES = 512,
    RENDER_IDLE_MAXIMUM_OVERLAY_IMAGE_PREWARMS = 4,
    RENDER_IDLE_MAXIMUM_OVERFLOW_IMAGE_PREWARMS = 4,
    RENDER_IDLE_MAXIMUM_GLYPH_CANDIDATES = 512,
    RENDER_IDLE_MAXIMUM_GLYPH_PREWARMS = 64,
    RENDER_IDLE_MAXIMUM_IMAGE_PIXELS = 8192,
    RENDER_IDLE_MAXIMUM_ENCODED_IMAGE_BYTES = 32 * 1024
};

static void tile_cache_idle_begin_glyphs(TileCache *cache)
{
    RenderIdleWork *work = &cache->idle_work;
    if (cache->idle_glyph_warming_disabled
        || cache->glyph_cache_capacity == 0
        || cache->layout->paint_order_count != cache->layout->count) {
        work->stage = RENDER_IDLE_WORK_COMPLETE;
        return;
    }
    int first = work->tile_y;
    int rows = (cache->layout->viewport.device_height
                + TILEFINCH_TILE_SIZE - 1) / TILEFINCH_TILE_SIZE;
    if (rows < 1) rows = 1;
    if (cache->layout->spatial_band_offsets != NULL && first >= 0
        && (size_t) first < cache->layout->spatial_band_count) {
        size_t last = (size_t) first + (size_t) rows - 1u;
        if (last >= cache->layout->spatial_band_count) {
            last = cache->layout->spatial_band_count - 1u;
        }
        work->glyph_band = (size_t) first;
        work->glyph_last_band = last;
        work->glyph_at = cache->layout->spatial_band_offsets[first];
        work->glyph_end = cache->layout->spatial_band_offsets[first + 1];
        work->stage = RENDER_IDLE_WORK_GLYPH_BAND;
    } else {
        work->glyph_fallback_order = 0;
        work->stage = RENDER_IDLE_WORK_GLYPH_FALLBACK;
    }
}

static bool idle_image_admitted(TileCache *cache,
                                const ImageResource *image,
                                const DrawCommand *command)
{
    if (cache == NULL || image == NULL || command == NULL
        || image->width <= 0 || image->height <= 0
        || command->width <= 0 || command->height <= 0
        || (size_t) image->width > SIZE_MAX / (size_t) image->height
        || (size_t) command->width > SIZE_MAX / (size_t) command->height) {
        return false;
    }
    size_t source_pixels = (size_t) image->width * (size_t) image->height;
    size_t target_pixels = (size_t) command->width * (size_t) command->height;
    bool admitted = source_pixels <= RENDER_IDLE_MAXIMUM_IMAGE_PIXELS
        && target_pixels <= RENDER_IDLE_MAXIMUM_IMAGE_PIXELS
        && (image->pixels != NULL
            || image->encoded_length
                   <= RENDER_IDLE_MAXIMUM_ENCODED_IMAGE_BYTES);
    if (cache->idle_work.allow_large_images) admitted = true;
    if (!admitted) cache->idle_image_admission_skips++;
    return admitted;
}

static bool prefetch_command_image(TileCache *cache,
                                   const DrawCommand *command,
                                   bool overflow)
{
    if (cache == NULL || command == NULL || command->image == NULL
        || !image_resource_available(command->image)
        || !idle_image_admitted(cache, command->image, command)) return false;
    size_t builds_before = cache->decoded_image_builds
        + cache->scaled_image_builds;
    int pixel_width = 0, pixel_height = 0;
    unsigned char *temporary = NULL;
    (void) image_pixels_for_command(
        cache, command->image, command, &pixel_width, &pixel_height,
        &temporary);
    image_resource_free_decoded(cache->budget, temporary);
    if (cache->decoded_image_builds + cache->scaled_image_builds
        > builds_before) {
        if (overflow) cache->overflow_images_prewarmed++;
        else cache->overlay_images_prewarmed++;
    }
    return true;
}

static bool prefetch_overflow_image(
    TileCache *cache, size_t order, int band_top, int band_bottom,
    size_t *warmed)
{
    if (cache == NULL || warmed == NULL
        || order >= cache->layout->paint_order_count) return false;
    size_t index = cache->layout->paint_order[order];
    if (index >= cache->layout->count
        || !(cache->layout->command_flags[index]
             & LAYOUT_COMMAND_OVERFLOW)) return false;
    const DrawCommand *command = &cache->layout->commands[index];
    int64_t command_bottom = (int64_t) command->y + command->height;
    if (command->image == NULL
        || command->y >= band_bottom
        || command_bottom <= band_top
        || !image_resource_available(command->image)) return false;
    (void) prefetch_command_image(cache, command, true);
    (*warmed)++;
    return true;
}

void tile_cache_cancel_idle_work(TileCache *cache)
{
    if (cache == NULL || !cache->idle_work.pending) return;
    cache->idle_jobs_cancelled++;
    memset(&cache->idle_work, 0, sizeof(cache->idle_work));
}

bool tile_cache_idle_work_pending(const TileCache *cache)
{
    return cache != NULL && cache->idle_work.pending;
}

static void tile_cache_complete_idle_work(TileCache *cache)
{
    cache->idle_jobs_completed++;
    cache->prefetch_rows++;
    memset(&cache->idle_work, 0, sizeof(cache->idle_work));
}

static void tile_cache_idle_begin_overflow(TileCache *cache)
{
    RenderIdleWork *work = &cache->idle_work;
    if (cache->layout->command_flags == NULL
        || cache->layout->paint_order_count != cache->layout->count) {
        tile_cache_idle_begin_glyphs(cache);
        return;
    }
    if (cache->layout->spatial_band_offsets != NULL
        && work->tile_y >= 0
        && (size_t) work->tile_y < cache->layout->spatial_band_count) {
        work->band_at =
            cache->layout->spatial_band_offsets[work->tile_y];
        work->band_end =
            cache->layout->spatial_band_offsets[work->tile_y + 1];
        work->stage = RENDER_IDLE_WORK_OVERFLOW_BAND;
    } else {
        work->fallback_order = 0;
        work->stage = RENDER_IDLE_WORK_OVERFLOW_FALLBACK;
    }
}

static void tile_cache_idle_advance_overlay_range(TileCache *cache)
{
    RenderIdleWork *work = &cache->idle_work;
    while (work->stage == RENDER_IDLE_WORK_OVERLAY_STICKY
           && work->range_index < cache->layout->sticky_count) {
        const StickyRange *range =
            &cache->layout->sticky_ranges[work->range_index];
        if (work->command_index < range->command_start) {
            work->command_index = range->command_start;
        }
        if (work->command_index < range->command_end
            && work->command_index < cache->layout->count) return;
        work->range_index++;
        work->command_index = 0;
    }
    if (work->stage == RENDER_IDLE_WORK_OVERLAY_STICKY) {
        work->stage = RENDER_IDLE_WORK_OVERLAY_FIXED;
        work->range_index = 0;
        work->command_index = 0;
    }
    while (work->stage == RENDER_IDLE_WORK_OVERLAY_FIXED
           && work->range_index < cache->layout->fixed_count) {
        const FixedRange *range =
            &cache->layout->fixed_ranges[work->range_index];
        if (work->command_index < range->command_start) {
            work->command_index = range->command_start;
        }
        if (work->command_index < range->command_end
            && work->command_index < cache->layout->count) return;
        work->range_index++;
        work->command_index = 0;
    }
    if (work->stage == RENDER_IDLE_WORK_OVERLAY_FIXED) {
        cache->overlay_images_prewarm_complete = true;
        tile_cache_idle_begin_overflow(cache);
    }
}

void tile_cache_schedule_prefetch_row(TileCache *cache, int world_y,
                                      int viewport_width)
{
    if (cache == NULL || cache->layout == NULL
        || cache->source_layout == NULL || world_y < 0
        || viewport_width <= 0) return;
    world_y = viewport_css_to_device(&cache->source_layout->viewport,
                                     world_y);
    int tile_y = world_y / TILEFINCH_TILE_SIZE;
    if (cache->idle_work.pending
        && cache->idle_work.tile_y == tile_y
        && cache->idle_work.viewport_width == viewport_width) return;
    tile_cache_cancel_idle_work(cache);
    uint64_t generation = cache->idle_work.generation + 1u;
    cache->idle_work = (RenderIdleWork) {
        .stage = RENDER_IDLE_WORK_TILES,
        .tile_y = tile_y,
        .viewport_width = viewport_width,
        .last_tile_x = (viewport_width - 1) / TILEFINCH_TILE_SIZE,
        .generation = generation,
        .pending = true
    };
    cache->idle_jobs_scheduled++;
}

static void tile_cache_run_overlay_unit(TileCache *cache)
{
    RenderIdleWork *work = &cache->idle_work;
    size_t scanned = 0;
    while (work->pending && scanned < RENDER_IDLE_SCAN_UNIT
           && work->overlay_candidates
                  < RENDER_IDLE_MAXIMUM_OVERLAY_CANDIDATES
           && work->overlay_images_warmed
                  < RENDER_IDLE_MAXIMUM_OVERLAY_IMAGE_PREWARMS
           && (work->stage == RENDER_IDLE_WORK_OVERLAY_STICKY
               || work->stage == RENDER_IDLE_WORK_OVERLAY_FIXED)) {
        tile_cache_idle_advance_overlay_range(cache);
        if (work->stage != RENDER_IDLE_WORK_OVERLAY_STICKY
            && work->stage != RENDER_IDLE_WORK_OVERLAY_FIXED) break;
        const DrawCommand *command =
            &cache->layout->commands[work->command_index++];
        work->overlay_candidates++;
        scanned++;
        if (prefetch_command_image(cache, command, false)) {
            work->overlay_images_warmed++;
            break;
        }
    }
    if (work->stage == RENDER_IDLE_WORK_OVERLAY_STICKY
        || work->stage == RENDER_IDLE_WORK_OVERLAY_FIXED) {
        if (work->overlay_candidates
                >= RENDER_IDLE_MAXIMUM_OVERLAY_CANDIDATES
            || work->overlay_images_warmed
                >= RENDER_IDLE_MAXIMUM_OVERLAY_IMAGE_PREWARMS) {
            cache->overlay_images_prewarm_complete = true;
            tile_cache_idle_begin_overflow(cache);
        } else {
            tile_cache_idle_advance_overlay_range(cache);
        }
    }
}

static void tile_cache_run_overflow_unit(TileCache *cache)
{
    RenderIdleWork *work = &cache->idle_work;
    int band_top = work->tile_y * TILEFINCH_TILE_SIZE;
    int band_bottom = band_top + TILEFINCH_TILE_SIZE;
    size_t scanned = 0;
    while (work->pending && scanned < RENDER_IDLE_SCAN_UNIT
           && work->overflow_candidates
                  < RENDER_IDLE_MAXIMUM_OVERFLOW_CANDIDATES
           && work->overflow_images_warmed
                  < RENDER_IDLE_MAXIMUM_OVERFLOW_IMAGE_PREWARMS) {
        size_t order = SIZE_MAX;
        if (work->stage == RENDER_IDLE_WORK_OVERFLOW_BAND) {
            if (work->band_at < work->band_end) {
                order = cache->layout->spatial_band_orders[work->band_at++];
            } else {
                work->stage = RENDER_IDLE_WORK_OVERFLOW_GLOBAL;
                continue;
            }
        } else if (work->stage == RENDER_IDLE_WORK_OVERFLOW_GLOBAL) {
            if (work->global_at < cache->layout->spatial_global_count) {
                order = cache->layout->spatial_global_orders[
                    work->global_at++];
            } else {
                tile_cache_idle_begin_glyphs(cache);
                break;
            }
        } else if (work->stage == RENDER_IDLE_WORK_OVERFLOW_FALLBACK) {
            if (work->fallback_order < cache->layout->count) {
                order = work->fallback_order++;
            } else {
                tile_cache_idle_begin_glyphs(cache);
                break;
            }
        } else {
            break;
        }
        work->overflow_candidates++;
        scanned++;
        if (order != SIZE_MAX && prefetch_overflow_image(
                cache, order, band_top, band_bottom,
                &work->overflow_images_warmed)) break;
    }
    if (work->overflow_candidates
            >= RENDER_IDLE_MAXIMUM_OVERFLOW_CANDIDATES
        || work->overflow_images_warmed
            >= RENDER_IDLE_MAXIMUM_OVERFLOW_IMAGE_PREWARMS) {
        tile_cache_idle_begin_glyphs(cache);
    }
}

static bool tile_cache_prewarm_text_glyph(TileCache *cache,
                                          const DrawCommand *command,
                                          size_t *text_offset)
{
    if (cache == NULL || command == NULL || text_offset == NULL
        || command->type != DRAW_TEXT || command->text == NULL
        || *text_offset >= command->text_length) return false;
    bool bold_face = draw_uses_bold_face(command);
    const FontFace *face = font_context_face_variant(
        cache->layout->fonts, cache->layout->web_fonts,
        draw_command_font_family(command), draw_command_font_italic(command),
        bold_face);
    if (face == NULL) return false;
    unsigned codepoint = 0;
    const LayoutBidiCommand *bidi = NULL;
    uintptr_t command_address = (uintptr_t) command;
    uintptr_t commands_address = (uintptr_t) cache->layout->commands;
    size_t commands_bytes = cache->layout->count * sizeof(DrawCommand);
    if (commands_address <= UINTPTR_MAX - commands_bytes
        && command_address >= commands_address
        && command_address < commands_address + commands_bytes
        && (command_address - commands_address) % sizeof(DrawCommand) == 0) {
        bidi = layout_bidi_command_for_index(
            cache->layout,
            (command_address - commands_address) / sizeof(DrawCommand));
    }
    if (bidi != NULL) {
        size_t glyph_index = *text_offset;
        if (glyph_index >= bidi->glyph_count) return false;
        codepoint = cache->layout->bidi_glyphs[
            bidi->glyph_start + glyph_index].codepoint;
        glyph_index++;
        *text_offset = glyph_index >= bidi->glyph_count
            ? command->text_length : glyph_index;
    } else {
        size_t used = font_utf8_next(command->text + *text_offset,
                                     command->text_length - *text_offset,
                                     &codepoint);
        if (used == 0) return false;
        codepoint = draw_command_transform_codepoint(
            command, codepoint, *text_offset == 0);
        *text_offset += used;
    }
    unsigned weight = draw_font_weight(command);
    bool synthetic_bold = weight >= 550u
        && (weight < 650u
            || !font_context_face_is_bold(cache->layout->fonts,
                                          cache->layout->web_fonts, face));
    int pixel_height_fixed = draw_command_text_font_size_fixed(command);
    if (pixel_height_fixed > TILEFINCH_FONT_RASTER_PIXEL_LIMIT * 64) {
        pixel_height_fixed = TILEFINCH_FONT_RASTER_PIXEL_LIMIT * 64;
    }
    if (pixel_height_fixed <= 0) return false;
    size_t misses_before = cache->glyph_cache_misses;
    FontGlyph temporary = {0};
    bool smoothed = false;
    const FontGlyph *glyph = glyph_cache_acquire(
        cache, face, codepoint, pixel_height_fixed, synthetic_bold,
        &temporary, &smoothed);
    (void) smoothed;
    font_glyph_destroy(face, &temporary);
    if (glyph == NULL) return false;
    cache->idle_glyphs_prewarmed++;
    cache->idle_glyph_cache_misses +=
        cache->glyph_cache_misses - misses_before;
    return true;
}

static bool tile_cache_idle_next_glyph_order(TileCache *cache,
                                             size_t *order)
{
    RenderIdleWork *work = &cache->idle_work;
    for (;;) {
        if (work->stage == RENDER_IDLE_WORK_GLYPH_BAND) {
            if (work->glyph_at < work->glyph_end) {
                *order = cache->layout->spatial_band_orders[
                    work->glyph_at++];
                return true;
            }
            if (work->glyph_band < work->glyph_last_band) {
                work->glyph_band++;
                work->glyph_at = cache->layout->spatial_band_offsets[
                    work->glyph_band];
                work->glyph_end = cache->layout->spatial_band_offsets[
                    work->glyph_band + 1u];
                continue;
            }
            work->stage = RENDER_IDLE_WORK_GLYPH_GLOBAL;
            continue;
        }
        if (work->stage == RENDER_IDLE_WORK_GLYPH_GLOBAL) {
            if (work->glyph_global_at
                < cache->layout->spatial_global_count) {
                *order = cache->layout->spatial_global_orders[
                    work->glyph_global_at++];
                return true;
            }
            work->stage = RENDER_IDLE_WORK_COMPLETE;
            return false;
        }
        if (work->stage == RENDER_IDLE_WORK_GLYPH_FALLBACK) {
            if (work->glyph_fallback_order < cache->layout->count) {
                *order = work->glyph_fallback_order++;
                return true;
            }
            work->stage = RENDER_IDLE_WORK_COMPLETE;
            return false;
        }
        return false;
    }
}

static void tile_cache_run_glyph_unit(TileCache *cache)
{
    RenderIdleWork *work = &cache->idle_work;
    int band_top = work->tile_y * TILEFINCH_TILE_SIZE;
    int band_bottom = band_top + cache->layout->viewport.device_height;
    size_t scanned = 0;
    while (!work->glyph_have_command && scanned < RENDER_IDLE_SCAN_UNIT
           && work->glyph_candidates
                  < RENDER_IDLE_MAXIMUM_GLYPH_CANDIDATES) {
        size_t order = SIZE_MAX;
        if (!tile_cache_idle_next_glyph_order(cache, &order)) break;
        work->glyph_candidates++;
        scanned++;
        if (order >= cache->layout->paint_order_count) continue;
        size_t index = cache->layout->paint_order[order];
        if (index >= cache->layout->count) continue;
        const DrawCommand *command = &cache->layout->commands[index];
        int64_t bottom = (int64_t) command->y + command->height;
        if (command->type != DRAW_TEXT || command->text_length == 0
            || command->y >= band_bottom || bottom <= band_top) continue;
        work->glyph_command_order = order;
        work->glyph_text_offset = 0;
        work->glyph_have_command = true;
    }
    if (work->glyph_have_command) {
        size_t index = cache->layout->paint_order[work->glyph_command_order];
        const DrawCommand *command = &cache->layout->commands[index];
        size_t offset_before = work->glyph_text_offset;
        if (tile_cache_prewarm_text_glyph(
                cache, command, &work->glyph_text_offset)) {
            work->glyphs_warmed++;
        }
        if (work->glyph_text_offset >= command->text_length
            || work->glyph_text_offset == offset_before) {
            work->glyph_have_command = false;
        }
    }
    if (work->glyph_candidates >= RENDER_IDLE_MAXIMUM_GLYPH_CANDIDATES
        || work->glyphs_warmed >= RENDER_IDLE_MAXIMUM_GLYPH_PREWARMS) {
        work->stage = RENDER_IDLE_WORK_COMPLETE;
        work->glyph_have_command = false;
    }
}

static void tile_cache_run_idle_unit(TileCache *cache)
{
    RenderIdleWork *work = &cache->idle_work;
    if (work->stage == RENDER_IDLE_WORK_OVERLAY_STICKY
        || work->stage == RENDER_IDLE_WORK_OVERLAY_FIXED) {
        tile_cache_run_overlay_unit(cache);
    } else if (work->stage == RENDER_IDLE_WORK_OVERFLOW_BAND
               || work->stage == RENDER_IDLE_WORK_OVERFLOW_GLOBAL
               || work->stage == RENDER_IDLE_WORK_OVERFLOW_FALLBACK) {
        tile_cache_run_overflow_unit(cache);
    } else if (work->stage == RENDER_IDLE_WORK_GLYPH_BAND
               || work->stage == RENDER_IDLE_WORK_GLYPH_GLOBAL
               || work->stage == RENDER_IDLE_WORK_GLYPH_FALLBACK) {
        tile_cache_run_glyph_unit(cache);
    } else if (work->stage == RENDER_IDLE_WORK_TILES) {
        if (work->next_tile_x <= work->last_tile_x) {
            (void) ensure_tile(cache, work->next_tile_x++, work->tile_y);
        }
        if (work->next_tile_x > work->last_tile_x) {
            if (!cache->overlay_images_prewarm_complete) {
                work->stage = RENDER_IDLE_WORK_OVERLAY_STICKY;
                work->range_index = 0;
                work->command_index = 0;
                tile_cache_idle_advance_overlay_range(cache);
            } else {
                tile_cache_idle_begin_overflow(cache);
            }
        }
    } else if (work->stage == RENDER_IDLE_WORK_COMPLETE) {
        tile_cache_complete_idle_work(cache);
    } else {
        work->stage = RENDER_IDLE_WORK_COMPLETE;
    }
}

bool tile_cache_run_idle_work(TileCache *cache, uint64_t budget_us,
                              size_t maximum_units)
{
    if (cache == NULL || !cache->idle_work.pending || budget_us == 0
        || maximum_units == 0) return false;
    uint64_t started = render_now_us();
    bool startup = cache->idle_work.startup;
    size_t units = 0;
    while (cache->idle_work.pending && units < maximum_units) {
        uint64_t unit_started = render_now_us();
        tile_cache_run_idle_unit(cache);
        uint64_t unit_elapsed = render_now_us() - unit_started;
        if (startup) {
            if (unit_elapsed > cache->max_startup_visual_unit_us) {
                cache->max_startup_visual_unit_us = unit_elapsed;
            }
        } else if (unit_elapsed > cache->max_idle_unit_us) {
            cache->max_idle_unit_us = unit_elapsed;
        }
        units++;
        if (render_now_us() - started >= budget_us) {
            if (cache->idle_work.pending) cache->idle_budget_exhaustions++;
            break;
        }
    }
    uint64_t elapsed = render_now_us() - started;
    if (startup) {
        cache->startup_visual_slices++;
        cache->startup_visual_us += elapsed;
        if (elapsed > cache->max_startup_visual_slice_us) {
            cache->max_startup_visual_slice_us = elapsed;
        }
    } else {
        cache->idle_slices++;
        cache->idle_units += units;
        cache->idle_us += elapsed;
        if (elapsed > cache->max_idle_slice_us) {
            cache->max_idle_slice_us = elapsed;
        }
        if (elapsed > budget_us) cache->idle_slice_overruns++;
    }
    /* Preserve the older names in diagnostics while callers migrate. */
    cache->prefetch_us += elapsed;
    if (elapsed > cache->max_prefetch_us) cache->max_prefetch_us = elapsed;
    return cache->idle_work.pending;
}

void tile_cache_prepare_startup_visuals(TileCache *cache, int first_world_y,
                                        int viewport_width,
                                        uint64_t total_budget_us,
                                        size_t maximum_rows)
{
    if (cache == NULL || cache->source_layout == NULL || first_world_y < 0
        || viewport_width <= 0 || total_budget_us == 0
        || maximum_rows == 0) return;
    int css_step = viewport_device_to_css(
        &cache->source_layout->viewport, TILEFINCH_TILE_SIZE);
    if (css_step < 1) css_step = 1;
    uint64_t started = render_now_us();
    int world_y = first_world_y;
    for (size_t row = 0; row < maximum_rows; row++) {
        if (world_y >= cache->source_layout->height) break;
        uint64_t elapsed = render_now_us() - started;
        if (elapsed >= total_budget_us) break;
        tile_cache_schedule_prefetch_row(cache, world_y, viewport_width);
        if (!cache->idle_work.pending) break;
        cache->idle_work.startup = true;
        cache->idle_work.allow_large_images = true;
        (void) tile_cache_run_idle_work(
            cache, total_budget_us - elapsed, 32);
        if (cache->idle_work.pending) break;
        if (world_y > INT_MAX - css_step) break;
        world_y += css_step;
    }
    if (cache->idle_work.pending) {
        /*
         * The startup allowance is only a policy for the synchronous
         * first-paint window.  Keep the exact tile/range/glyph cursor so the
         * ordinary idle pump can resume it instead of rescanning the row.
         * Completed tiles and cache admissions were already retained; the
         * remaining units continue under the normal post-paint image bound.
         */
        cache->idle_work.startup = false;
        cache->idle_work.allow_large_images = false;
    }
}

void tile_cache_prefetch_row(TileCache *cache, int world_y, int viewport_width)
{
    tile_cache_schedule_prefetch_row(cache, world_y, viewport_width);
    while (tile_cache_idle_work_pending(cache)) {
        (void) tile_cache_run_idle_work(
            cache, UINT64_MAX / 2u, 2048);
    }
}

void tile_cache_invalidate_rect(TileCache *cache, int left, int top,
                                int right, int bottom)
{
    if (cache == NULL || cache->tiles == NULL || right <= left
        || bottom <= top) return;
    tile_cache_cancel_frame_work(cache);
    for (size_t i = 0; i < cache->tile_capacity; i++) {
        RenderTile *tile = &cache->tiles[i];
        if (!tile->valid) continue;
        int tile_left = tile->tile_x * TILEFINCH_TILE_SIZE;
        int tile_top = tile->tile_y * TILEFINCH_TILE_SIZE;
        if (tile_left < right && tile_left + TILEFINCH_TILE_SIZE > left
            && tile_top < bottom && tile_top + TILEFINCH_TILE_SIZE > top) {
            tile->valid = false;
        }
    }
    cache->invalidations++;
}

bool tile_cache_sync_layout_paint(TileCache *cache, int left, int top,
                                  int right, int bottom)
{
    if (cache == NULL || cache->source_layout == NULL || cache->layout == NULL
        || cache->source_layout->count != cache->layout->count) return false;
    if (cache->owns_visual_layout) {
        for (size_t i = 0; i < cache->source_layout->count; i++) {
            cache->visual_layout.commands[i].color =
                cache->source_layout->commands[i].color;
            cache->visual_layout.commands[i].opacity_scale =
                cache->source_layout->commands[i].opacity_scale;
        }
    }
    int visual_left = viewport_css_to_device(
        &cache->source_layout->viewport, left);
    int visual_top = viewport_css_to_device(
        &cache->source_layout->viewport, top);
    int visual_right = viewport_css_to_device(
        &cache->source_layout->viewport, right) + 1;
    int visual_bottom = viewport_css_to_device(
        &cache->source_layout->viewport, bottom) + 1;
    overflow_cache_destroy(cache);
    cache->fixed_ready = false;
    cache->fixed_backdrop = false;
    cache->fixed_backdrop_masked = false;
    tile_cache_invalidate_rect(
        cache, visual_left, visual_top, visual_right, visual_bottom);
    cache->canvas_paint_pending = true;
    if (cache->canvas_overlay_ready) {
        CanvasFastFrameCandidate candidate;
        if (!canvas_fast_frame_candidate_impl(
                cache, cache->canvas_overlay_scroll_y,
                cache->canvas_overlay_viewport_width,
                cache->canvas_overlay_viewport_height, false, &candidate)
            || !canvas_overlay_patch(
                cache, &candidate, visual_left, visual_top,
                visual_right, visual_bottom)) {
            canvas_overlay_cache_clear(cache);
        }
    }
    return true;
}

bool tile_cache_sync_canvas_paint(TileCache *cache, int left, int top,
                                  int right, int bottom)
{
    if (cache == NULL || cache->source_layout == NULL || cache->layout == NULL
        || cache->source_layout->count != cache->layout->count) return false;
    int visual_left = viewport_css_to_device(
        &cache->source_layout->viewport, left);
    int visual_top = viewport_css_to_device(
        &cache->source_layout->viewport, top);
    int visual_right = viewport_css_to_device(
        &cache->source_layout->viewport, right) + 1;
    int visual_bottom = viewport_css_to_device(
        &cache->source_layout->viewport, bottom) + 1;
    /* Overflow geometry and ordinary fixed overlays depend on layout, not on
       mutable canvas pixels. A backdrop-filter is the exception because its
       retained pixels sample the page underneath. */
    bool retained_overlay_samples_canvas =
        layout_has_css_backdrop_filter(cache->layout);
    for (size_t range = 0;
         !retained_overlay_samples_canvas
             && range < cache->layout->fixed_count;
         range++) {
        const FixedRange *fixed = &cache->layout->fixed_ranges[range];
        size_t end = fixed->command_end < cache->layout->count
            ? fixed->command_end : cache->layout->count;
        for (size_t i = fixed->command_start; i < end; i++) {
            const DrawCommand *command = &cache->layout->commands[i];
            if (command->type == DRAW_IMAGE && command->image != NULL
                && command->image->is_canvas) {
                retained_overlay_samples_canvas = true;
                break;
            }
        }
    }
    if (retained_overlay_samples_canvas) {
        cache->fixed_ready = false;
        cache->fixed_backdrop = false;
        cache->fixed_backdrop_masked = false;
    }
    tile_cache_invalidate_rect(
        cache, visual_left, visual_top, visual_right, visual_bottom);
    cache->canvas_paint_pending = true;
    return true;
}

void tile_cache_invalidate_image_identity(TileCache *cache,
                                          const void *identity)
{
    if (cache == NULL || identity == NULL) return;
    for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++) {
        if (cache->decoded_images[i].valid
            && cache->decoded_images[i].identity == identity) {
            decoded_image_evict(cache, i);
        }
        if (cache->scaled_images[i].valid
            && cache->scaled_images[i].identity == identity) {
            scaled_image_evict(cache, i);
        }
    }
}

static void tile_cache_clear_layout_transients(TileCache *cache)
{
    scaled_image_clear(cache);
    decoded_image_clear(cache);
    budget_free(cache->budget, cache->gradient_cache);
    budget_free(cache->budget, cache->fixed_alpha);
    budget_free(cache->budget, cache->fixed_pixels);
    budget_free(cache->budget, cache->fixed_row_last);
    budget_free(cache->budget, cache->fixed_row_first);
    cache->fixed_alpha = NULL;
    cache->gradient_cache = NULL;
    cache->fixed_pixels = NULL;
    cache->fixed_row_first = NULL;
    cache->fixed_row_last = NULL;
    cache->fixed_ready = false;
    cache->fixed_backdrop = false;
    cache->fixed_backdrop_masked = false;
    cache->fixed_cache_bytes = 0;
}

size_t tile_cache_reclaim_optional(TileCache *cache)
{
    if (cache == NULL || cache->budget == NULL) return 0;
    size_t before = budget_remaining(cache->budget);
    /*
     * Idle prefetch is speculative.  Frame work is deliberately retained:
     * input is only offered after a published frame, and a caller which has
     * explicitly begun a foreground frame remains responsible for cancelling
     * it through the normal cancellation API.
     */
    tile_cache_cancel_idle_work(cache);
    tile_cache_clear_layout_transients(cache);
    canvas_overlay_cache_clear(cache);
    glyph_cache_clear(cache, true);
    overflow_cache_destroy(cache);
    cache->overflow_cache_disabled = false;
    size_t after = budget_remaining(cache->budget);
    return after > before ? after - before : 0;
}

static bool tile_cache_stage_layout(TileCache *cache,
                                    const LayoutDocument *layout,
                                    LayoutDocument *visual)
{
    if (cache == NULL || layout == NULL || visual == NULL) return false;
    memset(visual, 0, sizeof(*visual));
    if (viewport_context_is_scaled(&layout->viewport)) {
        if (!layout_clone_visual(visual, layout)) return false;
    }
    return true;
}

static void tile_cache_commit_layout(TileCache *cache,
                                     const LayoutDocument *layout,
                                     LayoutDocument *visual)
{
    tile_cache_cancel_idle_work(cache);
    const FontSet *old_fonts = cache->source_layout != NULL
                               ? cache->source_layout->fonts : NULL;
    const WebFontSet *old_web_fonts = cache->source_layout != NULL
                                      ? cache->source_layout->web_fonts : NULL;
    overflow_cache_destroy(cache);
    cache->overflow_cache_disabled = false;
    tile_cache_cancel_frame_work(cache);
    cache->last_frame_scroll_valid = false;
    tile_cache_release_visual_layout(cache);
    cache->source_layout = layout;
    if (visual->budget != NULL) {
        cache->visual_layout = *visual;
        memset(visual, 0, sizeof(*visual));
        cache->layout = &cache->visual_layout;
        cache->owns_visual_layout = true;
    } else {
        cache->layout = layout;
        cache->owns_visual_layout = false;
    }
    tile_cache_clear_layout_transients(cache);
    /* Page font storage is navigation-owned and may be freed and then reused
       at the same address.  Never retain pointer-keyed glyphs across a layout
       replacement involving web fonts, even when the pointer compares equal. */
    if (old_fonts != layout->fonts || old_web_fonts != NULL
        || layout->web_fonts != NULL) {
        glyph_cache_clear(cache, true);
    }
}

bool tile_cache_replace_layout(TileCache *cache,
                               const LayoutDocument *layout)
{
    if (cache == NULL || layout == NULL) return false;
    LayoutDocument visual = {0};
    if (!tile_cache_stage_layout(cache, layout, &visual)) {
        return false;
    }
    tile_cache_commit_layout(cache, layout, &visual);
    canvas_overlay_cache_clear(cache);
    for (size_t i = 0; i < cache->tile_capacity; i++) {
        cache->tiles[i].valid = false;
    }
    cache->invalidations++;
    return true;
}

static bool canvas_fast_same_surface(
    const CanvasFastFrameCandidate *left,
    const CanvasFastFrameCandidate *right)
{
    return left->image == right->image
        && left->command.x == right->command.x
        && left->command.y == right->command.y
        && left->command.width == right->command.width
        && left->command.height == right->command.height
        && left->command.image_fit == right->command.image_fit
        && left->command.opacity_scale == right->command.opacity_scale
        && left->clip_left == right->clip_left
        && left->clip_top == right->clip_top
        && left->clip_right == right->clip_right
        && left->clip_bottom == right->clip_bottom;
}

bool tile_cache_replace_layout_damage(TileCache *cache,
                                      const LayoutDocument *layout,
                                      int left, int top,
                                      int right, int bottom)
{
    if (cache == NULL || layout == NULL) return false;
    bool preserve_canvas_overlay = cache->canvas_overlay_ready
        && cache->last_frame_scroll_valid
        && cache->canvas_overlay_scroll_y == cache->last_frame_scroll_y;
    CanvasFastFrameCandidate old_canvas;
    if (preserve_canvas_overlay
        && !canvas_fast_frame_candidate_impl(
            cache, cache->canvas_overlay_scroll_y,
            cache->canvas_overlay_viewport_width,
            cache->canvas_overlay_viewport_height, false, &old_canvas)) {
        preserve_canvas_overlay = false;
    }
    LayoutDocument visual = {0};
    if (!tile_cache_stage_layout(cache, layout, &visual)) {
        return false;
    }
    int visual_left = 0, visual_top = 0, visual_right = 0, visual_bottom = 0;
    if (right > left && bottom > top) {
        visual_left = viewport_css_to_device(&layout->viewport, left);
        visual_top = viewport_css_to_device(&layout->viewport, top);
        visual_right = viewport_css_to_device(&layout->viewport, right) + 1;
        visual_bottom = viewport_css_to_device(&layout->viewport, bottom) + 1;
    }
    tile_cache_commit_layout(cache, layout, &visual);
    if (preserve_canvas_overlay) {
        cache->last_frame_scroll_y = cache->canvas_overlay_scroll_y;
        cache->last_frame_scroll_valid = true;
        CanvasFastFrameCandidate new_canvas;
        if (!canvas_fast_frame_candidate_impl(
                cache, cache->canvas_overlay_scroll_y,
                cache->canvas_overlay_viewport_width,
                cache->canvas_overlay_viewport_height, false, &new_canvas)
            || !canvas_fast_same_surface(&old_canvas, &new_canvas)
            || !canvas_overlay_patch(
                cache, &new_canvas, visual_left, visual_top,
                visual_right, visual_bottom)) {
            canvas_overlay_cache_clear(cache);
            cache->last_frame_scroll_valid = false;
        } else {
            cache->canvas_paint_pending = true;
        }
    } else {
        canvas_overlay_cache_clear(cache);
    }
    if (right > left && bottom > top) {
        tile_cache_invalidate_rect(cache, visual_left, visual_top,
                                   visual_right, visual_bottom);
    }
    if (preserve_canvas_overlay && cache->canvas_overlay_ready
        && cache->last_frame_scroll_valid) {
        cache->canvas_paint_pending = true;
    }
    return true;
}
