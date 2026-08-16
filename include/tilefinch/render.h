#ifndef TILEFINCH_RENDER_H
#define TILEFINCH_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/cancellation.h"
#include "tilefinch/layout.h"

#define TILEFINCH_TILE_SIZE LAYOUT_SPATIAL_BAND_HEIGHT
#define TILEFINCH_GLYPH_CACHE_ENTRIES 512
#define TILEFINCH_GLYPH_CACHE_WAYS 4
#define TILEFINCH_GLYPH_CACHE_BYTES (192u * 1024u)
#define TILEFINCH_IMAGE_CACHE_ENTRIES 4
#define TILEFINCH_DECODED_IMAGE_CACHE_BYTES (512u * 1024u)
#define TILEFINCH_SCALED_IMAGE_CACHE_BYTES (512u * 1024u)
#define TILEFINCH_OVERFLOW_CACHE_BYTES (256u * 1024u)

_Static_assert(TILEFINCH_GLYPH_CACHE_ENTRIES % TILEFINCH_GLYPH_CACHE_WAYS == 0,
               "glyph cache entries must divide into complete sets");
_Static_assert(((TILEFINCH_GLYPH_CACHE_ENTRIES / TILEFINCH_GLYPH_CACHE_WAYS)
                & (TILEFINCH_GLYPH_CACHE_ENTRIES / TILEFINCH_GLYPH_CACHE_WAYS - 1))
               == 0,
               "glyph cache set count must be a power of two");

typedef struct {
    bool valid;
    int tile_x;
    int tile_y;
    uint64_t last_used;
    uint16_t pixels[TILEFINCH_TILE_SIZE * TILEFINCH_TILE_SIZE];
} RenderTile;

typedef struct {
    const FontFace *face;
    unsigned codepoint;
    int pixel_height;
    bool bold;
    bool valid;
    bool smoothed;
    uint64_t last_used;
    size_t bytes;
    FontGlyph glyph;
} GlyphCacheEntry;

typedef struct {
    const void *identity;
    unsigned char *pixels;
    int width;
    int height;
    size_t bytes;
    uint64_t last_used;
    bool valid;
    bool failed;
} DecodedImageCacheEntry;

typedef struct {
    const void *identity;
    unsigned char *pixels;
    int source_width;
    int source_height;
    int width;
    int height;
    size_t bytes;
    uint64_t last_used;
    bool valid;
    bool failed;
} ScaledImageCacheEntry;

typedef enum {
    RENDER_IDLE_WORK_NONE = 0,
    RENDER_IDLE_WORK_OVERLAY_STICKY,
    RENDER_IDLE_WORK_OVERLAY_FIXED,
    RENDER_IDLE_WORK_OVERFLOW_BAND,
    RENDER_IDLE_WORK_OVERFLOW_GLOBAL,
    RENDER_IDLE_WORK_OVERFLOW_FALLBACK,
    RENDER_IDLE_WORK_GLYPH_BAND,
    RENDER_IDLE_WORK_GLYPH_GLOBAL,
    RENDER_IDLE_WORK_GLYPH_FALLBACK,
    RENDER_IDLE_WORK_TILES,
    RENDER_IDLE_WORK_COMPLETE
} RenderIdleWorkStage;

/* One latest-wins speculative job. Keeping the cursor inline avoids a heap
   queue and makes cancellation constant-time on the PSP input path. */
typedef struct {
    RenderIdleWorkStage stage;
    int tile_y;
    int viewport_width;
    int last_tile_x;
    int next_tile_x;
    size_t range_index;
    size_t command_index;
    size_t band_at;
    size_t band_end;
    size_t global_at;
    size_t fallback_order;
    size_t overlay_candidates;
    size_t overflow_candidates;
    size_t overlay_images_warmed;
    size_t overflow_images_warmed;
    size_t glyph_band;
    size_t glyph_last_band;
    size_t glyph_at;
    size_t glyph_end;
    size_t glyph_global_at;
    size_t glyph_fallback_order;
    size_t glyph_command_order;
    size_t glyph_text_offset;
    size_t glyph_candidates;
    size_t glyphs_warmed;
    uint64_t generation;
    bool pending;
    bool startup;
    bool allow_large_images;
    bool glyph_have_command;
} RenderIdleWork;

typedef enum {
    RENDER_FRAME_WORK_CANCELLED = -2,
    RENDER_FRAME_WORK_FAILED = -1,
    RENDER_FRAME_WORK_PENDING = 0,
    RENDER_FRAME_WORK_READY = 1
} RenderFrameWorkResult;

/*
 * One latest-wins foreground preparation job. It admits only the resident
 * tiles that the final frame compositor will preserve, so a 480x272 frame
 * remains bounded by the configured tile capacity instead of allocating a
 * second framebuffer or a visible-tile queue.
 */
typedef struct {
    int scroll_y;
    int viewport_width;
    int viewport_height;
    int first_tx;
    int first_ty;
    size_t columns;
    size_t required_tiles;
    size_t next_ordinal;
    size_t end_ordinal;
    bool pending;
    bool ready;
} RenderFrameWork;

typedef struct {
    Budget *budget;
    const LayoutDocument *layout;
    const LayoutDocument *source_layout;
    LayoutDocument visual_layout;
    bool owns_visual_layout;
    RenderTile *tiles;
    RenderTile *frame_scratch_tile;
    size_t tile_capacity;
    uint64_t clock;
    size_t hits;
    size_t misses;
    size_t evictions;
    size_t rasterized;
    uint16_t *frame;
    size_t frame_pixels;
    DecodedImageCacheEntry decoded_images[TILEFINCH_IMAGE_CACHE_ENTRIES];
    ScaledImageCacheEntry scaled_images[TILEFINCH_IMAGE_CACHE_ENTRIES];
    size_t decoded_image_cache_bytes;
    size_t scaled_image_cache_bytes;
    GlyphCacheEntry *glyph_cache;
    size_t glyph_cache_capacity;
    size_t glyph_cache_count;
    size_t glyph_cache_bytes;
    size_t glyph_cache_hits;
    size_t glyph_cache_misses;
    size_t glyph_cache_evictions;
    size_t scaled_image_hits;
    size_t scaled_image_builds;
    size_t scaled_image_evictions;
    size_t decoded_image_hits;
    size_t decoded_image_builds;
    size_t decoded_image_evictions;
    size_t decoded_image_failures;
    uint64_t decoded_image_us;
    uint64_t max_decoded_image_us;
    uint64_t scaled_image_us;
    uint64_t max_scaled_image_us;
    uint16_t *fixed_pixels;
    uint8_t *fixed_alpha;
    uint16_t *fixed_row_first;
    uint16_t *fixed_row_last;
    int fixed_left;
    int fixed_top;
    int fixed_width;
    int fixed_height;
    int fixed_viewport_width;
    int fixed_viewport_height;
    bool fixed_ready;
    bool fixed_backdrop;
    bool fixed_backdrop_masked;
    size_t fixed_cache_builds;
    size_t fixed_cache_blits;
    size_t fixed_cache_pixels;
    size_t fixed_cache_bytes;
    size_t invalidations;
    uint64_t raster_us;
    uint64_t frame_us;
    uint64_t max_raster_us;
    uint64_t max_frame_us;
    /* Portion of frame_us spent writing frame captures (PPM output).  Product
       frame cost is frame_us - frame_io_us; the lab harness is the only
       writer on hosts, and the PSP frontend never sets an output path. */
    uint64_t frame_io_us;
    uint64_t frame_setup_us;
    uint64_t frame_tile_us;
    uint64_t frame_overflow_us;
    uint64_t frame_sticky_us;
    uint64_t frame_fixed_us;
    uint64_t frame_indicator_us;
    uint64_t max_frame_setup_us;
    uint64_t max_frame_tile_us;
    uint64_t max_frame_overflow_us;
    uint64_t max_frame_sticky_us;
    uint64_t max_frame_fixed_us;
    size_t frames_rendered;
    uint64_t command_candidates;
    struct RenderOverflowCache *overflow_cache;
    struct RenderGradientCache *gradient_cache;
    size_t gradient_lut_hits;
    size_t gradient_lut_builds;
    bool overflow_cache_disabled;
    bool fast_text_raster;
    size_t overflow_cache_bytes;
    size_t overflow_cache_builds;
    size_t overflow_cache_fallbacks;
    size_t overflow_direct_commands;
    size_t frame_preserved_tiles;
    uint64_t prefetch_us;
    uint64_t max_prefetch_us;
    size_t prefetch_rows;
    size_t overlay_images_prewarmed;
    size_t overflow_images_prewarmed;
    bool overlay_images_prewarm_complete;
    bool idle_glyph_warming_disabled;
    RenderIdleWork idle_work;
    RenderFrameWork frame_work;
    size_t frame_jobs_scheduled;
    size_t frame_jobs_completed;
    size_t frame_jobs_cancelled;
    size_t frame_job_slices;
    size_t frame_job_units;
    size_t frame_job_budget_exhaustions;
    size_t frame_job_slice_overruns;
    uint64_t frame_job_us;
    uint64_t max_frame_job_slice_us;
    uint64_t max_frame_job_unit_us;
    size_t idle_jobs_scheduled;
    size_t idle_jobs_completed;
    size_t idle_jobs_cancelled;
    size_t idle_slices;
    size_t idle_units;
    size_t idle_budget_exhaustions;
    size_t idle_slice_overruns;
    size_t idle_image_admission_skips;
    size_t idle_glyphs_prewarmed;
    size_t idle_glyph_cache_misses;
    uint64_t idle_us;
    uint64_t max_idle_slice_us;
    uint64_t max_idle_unit_us;
    size_t startup_visual_slices;
    uint64_t startup_visual_us;
    uint64_t max_startup_visual_slice_us;
    uint64_t max_startup_visual_unit_us;
    int last_frame_scroll_y;
    bool last_frame_scroll_valid;
    bool forced_dark;
} TileCache;

bool tile_cache_init(TileCache *cache, Budget *budget,
                     const LayoutDocument *layout, size_t tile_capacity);
bool tile_cache_set_frame(TileCache *cache, uint16_t *frame,
                          size_t frame_pixels);
/* Provisional-only quality tier: consume the font backend's native coverage
   directly instead of applying the final-paint expansion filter. Geometry,
   shaping, colors, and authored font selection remain unchanged. */
void tile_cache_set_fast_text_raster(TileCache *cache, bool enabled);
/*
 * Force a dark page palette at paint time. Authored raster content
 * (images, video-backed surfaces, and canvas snapshots) is deliberately
 * excluded: only CSS surfaces, ink, borders, gradients, and shadows are
 * remapped. Changing the mode invalidates retained paint products.
 */
void tile_cache_set_forced_dark(TileCache *cache, bool enabled);
/* Drops retained glyph products and every tile that may contain them. Used
   when a provider is detached; it performs no storage I/O. */
void tile_cache_invalidate_glyphs(TileCache *cache);
/* Repaints temporary fallback glyphs after a deferred provider block becomes
   resident while preserving already-rasterized glyphs. */
void tile_cache_repaint_glyphs(TileCache *cache);
void tile_cache_destroy(TileCache *cache);
bool tile_cache_render_frame(TileCache *cache, int scroll_y,
                             int viewport_width, int viewport_height,
                             const char *output_path);
RenderFrameWorkResult tile_cache_prepare_frame_bounded(
    TileCache *cache, int scroll_y, int viewport_width, int viewport_height,
    uint64_t budget_us, size_t maximum_units);
/*
 * The cancelable form checks before and after each raster unit. A rasterizer
 * already inside one tile is allowed to finish safely; its candidate frame
 * is not published by this function.
 */
RenderFrameWorkResult tile_cache_prepare_frame_bounded_cancelable(
    TileCache *cache, int scroll_y, int viewport_width, int viewport_height,
    uint64_t budget_us, size_t maximum_units,
    const TilefinchCancellation *cancellation);
void tile_cache_cancel_frame_work(TileCache *cache);
bool tile_cache_frame_work_pending(const TileCache *cache);
void render_paint_focus_outline(uint16_t *frame, size_t frame_pixels,
                                int viewport_width, int viewport_height,
                                int x, int y, int width, int height);
void render_paint_authored_focus_outline(
    uint16_t *frame, size_t frame_pixels,
    int viewport_width, int viewport_height,
    int x, int y, int width, int height,
    int outline_width, int outline_offset, unsigned outline_style,
    uint32_t color, uint8_t alpha);
/* Allocation-free compositor overlay used after cached page tiles have been
   copied. Current matches receive a stronger fill and border. */
void render_paint_find_highlight(
    uint16_t *frame, size_t frame_pixels,
    int viewport_width, int viewport_height,
    int x, int y, int width, int height, bool current);
bool render_write_frame_ppm(const char *path, const uint16_t *frame,
                            int width, int height);
void tile_cache_prefetch_row(TileCache *cache, int world_y, int viewport_width);
void tile_cache_schedule_prefetch_row(TileCache *cache, int world_y,
                                      int viewport_width);
void tile_cache_cancel_idle_work(TileCache *cache);
/*
 * Release rebuildable render accelerators without invalidating the current
 * page, resident tiles, or presented framebuffer.  The next frame lazily
 * recreates any glyph, image, fixed-overlay, or overflow data it needs.
 */
size_t tile_cache_reclaim_optional(TileCache *cache);
bool tile_cache_run_idle_work(TileCache *cache, uint64_t budget_us,
                              size_t maximum_units);
bool tile_cache_idle_work_pending(const TileCache *cache);
void tile_cache_prepare_startup_visuals(TileCache *cache, int first_world_y,
                                        int viewport_width,
                                        uint64_t total_budget_us,
                                        size_t maximum_rows);
void tile_cache_invalidate_rect(TileCache *cache, int left, int top,
                                int right, int bottom);
/* Mirror paint fields from an unchanged source display list into its scaled
   visual clone, then invalidate the CSS-space damage rectangle. */
bool tile_cache_sync_layout_paint(TileCache *cache, int left, int top,
                                  int right, int bottom);
/* Drop decoded/scaled derivatives for one stable mutable image surface. */
void tile_cache_invalidate_image_identity(TileCache *cache,
                                          const void *identity);
bool tile_cache_replace_layout(TileCache *cache,
                               const LayoutDocument *layout);
bool tile_cache_replace_layout_damage(TileCache *cache,
                                      const LayoutDocument *layout,
                                      int left, int top,
                                      int right, int bottom);

#endif
