#ifndef TILEFINCH_RENDER_H
#define TILEFINCH_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/cancellation.h"
#include "tilefinch/layout.h"

#define TILEFINCH_TILE_SIZE LAYOUT_SPATIAL_BAND_HEIGHT
/* Tiles allocated as ordinarily as the frame itself. Slots past this hold
   paint-ahead: optional, admitted with budget headroom, reclaimed first. */
#define RENDER_TILE_BASE_CAPACITY 8u
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
    /* TileCache.overflow_tile_generation when rasterized: a tile holding
       unscrolled overflow content is stale once any overflow box scrolls. */
    uint32_t overflow_generation;
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
    /* Uses pre-existing padding on the host and 32-bit PSP ABI. */
    uint8_t humanist_profile;
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
    /* Further paint-ahead rows after tile_y, stepping by row_step (+1 below
       the viewport, -1 above it), while spare tile slots allow. */
    size_t extra_rows;
    int row_step;
    int prefetch_scroll_y;
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

typedef enum {
    RENDER_CANVAS_FRAME_NOT_APPLICABLE = 0,
    RENDER_CANVAS_FRAME_COMPLETE = 1,
    RENDER_CANVAS_FRAME_FAILED = -1
} RenderCanvasFrameResult;

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
    /* Keep the source of an in-flight frame explicit across bounded slices.
       Animation scheduling uses this to finish the current canvas publish
       before admitting another requestAnimationFrame mutation. */
    bool canvas_paint;
} RenderFrameWork;

#define TILEFINCH_CANVAS_OVERLAY_REGION_LIMIT 8u

typedef struct {
    int16_t left;
    int16_t top;
    uint16_t width;
    uint16_t height;
    uint32_t pixel_offset;
} RenderCanvasOverlayRegion;

/* Deferred canvas publication. When a frontend that can scale the EDRAM
   WebGL surface itself (the PSP presenter) enables it, a complete,
   undecorated full-viewport 2:3 canvas frame whose WebGL surface is still
   authoritative in EDRAM is not converted into the RAM frame: the presenter
   scales it straight into the display back buffer with the GE, and the RAM
   copy is produced only when something reads it
   (tile_cache_canvas_materialize). Off unless a frontend enables it with
   tile_cache_set_canvas_defer. */
typedef struct {
    const unsigned char *source;
    size_t source_stride;
    int source_width;
    int source_height;
    /* Destination rectangle in frame pixels (the frame is the viewport). */
    int x;
    int y;
    int width;
    int height;
    /* Snapshot identity of the native surface (see
       image_canvas_native_serial); a newer canvas frame invalidates it. */
    uint32_t serial;
    uint32_t epoch;
} RenderCanvasDeferral;

/* Declared in every build because TileCache embeds it (see there). */
typedef struct {
    uint32_t kernel_rows, general_rows, repeat_rows, skipped_rows;
    uint32_t vfpu_rows;
    uint32_t source_pixels;
    uintptr_t source_address;
    size_t source_stride;
    int source_width, source_height, output_width, output_height;
    bool native_source;
    bool setup_once;
} RenderCanvasConversionMetrics;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* Measurement-only switch between the ordinary Allegrex kernel (the
   default, as in shipping builds) and its scalar reference. The probe
   compares output pixels and borrowed state; a failure turns the kernel
   off until a later probe passes. */
typedef struct {
    uint32_t rows, pixels, mismatches, state_mismatches;
    bool available;
} RenderCanvasVfpuProbe;
void tile_cache_validation_vfpu_conversion(bool enabled);
RenderCanvasVfpuProbe tile_cache_validation_probe_vfpu_conversion(void);
/* Measurement-only baseline control; production setup admission is default. */
void tile_cache_validation_canvas_setup(bool enabled);
bool tile_cache_validation_canvas_setup_probe(FILE *output);
#endif

#ifndef __PSP__
/* Host test hook: translucent blends into a layer with its own alpha,
   checked against the plain arithmetic for every destination alpha and
   source alpha over one destination and source colour. */
size_t render_test_alpha_blend_mismatches(uint16_t destination,
                                          uint32_t foreground);
#endif

typedef struct {
    Budget *budget;
    const LayoutDocument *layout;
    const LayoutDocument *source_layout;
    LayoutDocument visual_layout;
    bool owns_visual_layout;
    /* Slots, allocated on first use. The first
       RENDER_TILE_BASE_CAPACITY behave as before; later ones hold paint-ahead
       and are allocated only with budget headroom and released first. */
    RenderTile **tiles;
    size_t tiles_allocated;
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
    size_t fixed_cache_patches;
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
    uint64_t stroke_pixel_tests; /* Host/validation only; zero in ordinary PSP. */
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
    bool canvas_paint_pending;
    size_t canvas_fast_frames;
    size_t canvas_setup_frames;
    size_t canvas_fast_refusals;
    uint64_t canvas_fast_us;
    uint64_t canvas_fast_max_us;
    uint64_t canvas_fast_raster_us;
    uint64_t canvas_fast_overlay_us;
    /* Per frame, from the engine: nothing (find highlight, authored focus
       outline) will be painted into the frame after the canvas pass. */
    bool canvas_defer_allowed;
    /* The frame's canvas rectangle was not rasterized for this frame. */
    bool canvas_deferred;
    RenderCanvasDeferral canvas_deferral;
    size_t canvas_deferrals;
    size_t canvas_materializations;
    size_t canvas_materialize_failures;
    /* Last fast conversion's actual path, not an assumed scale ratio.
       Scalar row counts avoid clocks or logging inside the pixel loop.
       Filled only in validation builds, but always present: the PSP support
       libraries never see TILEFINCH_PSP_VALIDATION_LOG, and a public struct
       layout must not depend on it (src/abi_layout_probe.c). */
    RenderCanvasConversionMetrics canvas_fast_conversion;
    uint16_t *canvas_overlay_pixels;
    uint8_t *canvas_overlay_alpha;
    /* Blend tables for the retained overlay's most frequent translucent
       (colour, alpha) pairs, planned on the first blit after the overlay
       changes; NULL when not admitted. */
    struct CanvasOverlayBlendCache *canvas_overlay_blend_cache;
    RenderCanvasOverlayRegion
        canvas_overlay_regions[TILEFINCH_CANVAS_OVERLAY_REGION_LIMIT];
    size_t canvas_overlay_region_count;
    size_t canvas_overlay_pixel_count;
    int canvas_overlay_scroll_y;
    int canvas_overlay_viewport_width;
    int canvas_overlay_viewport_height;
    /* Source layout scroll_generation the overlay was rasterized at: an
       overflow box scrolling moves overlay content without a relayout. */
    uint32_t canvas_overlay_scroll_generation;
    bool canvas_overlay_ready;
    size_t canvas_overlay_builds;
    size_t canvas_overlay_patches;
    size_t canvas_overlay_patch_regions;
    /* Retained overlay pixels cleared and repainted by patches. */
    size_t canvas_overlay_patch_pixels;
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
    int last_frame_viewport_width;
    int last_frame_viewport_height;
    bool last_frame_scroll_valid;
    /* Scroll position of the last composed frame. Unlike last_frame_*,
       layout replacement keeps it: the frontend shows placeholder tiles
       only when the view moved, never for an in-place repaint. */
    int presented_scroll_y;
    bool presented_scroll_valid;
    /* Paint-ahead rows finished for the screen at prefetch_done_scroll_y;
       cleared by any tile invalidation. */
    bool prefetch_done;
    int prefetch_done_scroll_y;
    /* Placeholder composition: draw a checkerboard where a visible tile is
       not rasterized yet instead of rasterizing it, and leave the pending
       frame job running. placeholder_tiles counts them for the last frame. */
    bool placeholder_missing;
    size_t placeholder_tiles;
    /* When set, a one-shot full-frame render yields to the frontend under
       this cooperate phase whenever 8 ms of tile raster has passed (the
       load preview's first frame is ~0.1 s on the PSP). It never aborts the
       frame: a cancel request persists to the caller's next checkpoint. */
    const char *raster_cooperate_phase;
    uint64_t raster_slice_started_us;
    /* Bumped whenever which overflow content is baked into tiles may have
       changed: an overflow box scrolled, or the overflow cache was rebuilt
       or lost. */
    uint32_t overflow_tile_generation;
    uint64_t overflow_scroll_signature;
    /* The overflow offsets and clips of the fixed layer's commands when it
       was last validated: an unrelated scroller moving keeps the layer. */
    uint64_t fixed_overflow_signature;
    /* Overflow preparations answered by the scroll generation alone. */
    size_t overflow_prepare_skips;
    /* Last overflow-cache preparation succeeded, so tiles bake unscrolled
       overflow content. Survives a cache rebuild; flips only on success
       after failure or failure after success. */
    bool overflow_static_active;
    size_t overflow_tile_commands;
    bool forced_dark;
    /* Scroll state last mirrored into the visual layout, and last answer to
       "is any overflow box scrolled vertically", keyed by the source
       layout and its scroll_generation so idle frames skip both scans. */
    const LayoutDocument *scroll_synced_layout;
    uint32_t scroll_synced_generation;
    const LayoutDocument *overflow_scroll_layout;
    uint32_t overflow_scroll_generation;
    bool overflow_scroll_active;
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
/* An opaque, simply transformed canvas can replace its rectangle in the prior
   page pixels directly and then replay later page overlays. More complex
   canvas composition returns NOT_APPLICABLE and retains the ordinary tile
   path unchanged. */
RenderCanvasFrameResult tile_cache_render_canvas_frame_fast(
    TileCache *cache, int scroll_y, int viewport_width, int viewport_height);
/* Process-wide: only a frontend with a GE presenter turns this on. */
void tile_cache_set_canvas_defer(bool enabled);
bool tile_cache_canvas_defer_enabled(void);
/* The pending deferral for the current frame, or NULL when the RAM frame is
   complete. */
const RenderCanvasDeferral *tile_cache_canvas_deferral(
    const TileCache *cache);
/* Produce the deferred canvas pixels in the RAM frame (the exact CPU
   conversion the frame would have had). False when the surface no longer
   holds that frame; the frame is then marked for a full repaint and the
   deferral is dropped either way. True when nothing was deferred. */
bool tile_cache_canvas_materialize(TileCache *cache);
bool tile_cache_canvas_frame_fast_eligible(
    const TileCache *cache, int scroll_y,
    int viewport_width, int viewport_height);
/* maximum_units limits newly rasterized tiles, not resident cache hits.
   Cached-tile checks remain deadline- and viewport-bounded. */
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
bool tile_cache_canvas_frame_work_pending(const TileCache *cache);
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
                            size_t frame_pixels, int width, int height);
void tile_cache_prefetch_row(TileCache *cache, int world_y, int viewport_width);
void tile_cache_schedule_prefetch_row(TileCache *cache, int world_y,
                                      int viewport_width);
void tile_cache_cancel_idle_work(TileCache *cache);
/* True when the paint-ahead rows for the last presented screen are cached
   (an idle cancel before they finished, or an invalidation, clears it). */
bool tile_cache_prefetch_satisfied(const TileCache *cache);
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
/* Mutable canvas pixels do not change layout or overflow geometry. Preserve
   those accelerators while invalidating tiles that retain the old pixels. */
bool tile_cache_sync_canvas_paint(TileCache *cache, int left, int top,
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
