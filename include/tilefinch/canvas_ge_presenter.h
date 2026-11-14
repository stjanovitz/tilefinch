#ifndef TILEFINCH_CANVAS_GE_PRESENTER_H
#define TILEFINCH_CANVAS_GE_PRESENTER_H

/*
 * Publication policy for deferred canvas frames (render.h,
 * RenderCanvasDeferral): the platform-neutral half of the PSP's GE-direct
 * presenter, kept here so host tests drive the same decisions through a
 * fake backend.
 *
 * For each page presentation the frontend asks canvas_ge_presenter_publish()
 * whether the frame's canvas rows were published by the backend (the GE
 * scaled the EDRAM surface into the back buffer), whether the RAM frame is
 * complete and should be copied as usual, or whether this buffer must not be
 * published at all.
 *
 * Ownership rules the policy enforces:
 * - Only the owner thread may submit work or produce the RAM frame; a
 *   deferred frame offered by any other thread is refused, never torn.
 * - A submission whose completion is not established (error or bounded wait
 *   expired) latches the path off for the process, closes the shared GE
 *   context through the backend, stops further deferral, and refuses that
 *   buffer. While the backend still reports the old submission busy, every
 *   presentation is refused so the CPU cannot write a buffer the GE may
 *   still be writing; the last published frame stays on the panel.
 * - Anything ineligible, a refused submission or a disabled path falls back
 *   to the exact CPU conversion (tile_cache_canvas_materialize) and the
 *   ordinary copy. A surface that has moved on forces a full repaint
 *   instead of publishing stale pixels.
 * - The backend allocates its GE scratch on its first scale(). The presenter
 *   asks it to release that scratch once the page that used it is gone: the
 *   page generation (canvas_ge_presenter_set_page) has moved on and a
 *   present on the owner thread carries no deferral. Never while an earlier
 *   submission may still run; a backend that still reports busy is asked
 *   again on a later present.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/render.h"

typedef enum {
    CANVAS_GE_PRESENT_REFUSE = -1,
    CANVAS_GE_PRESENT_COPY = 0,
    CANVAS_GE_PRESENT_PUBLISHED = 1
} CanvasGePresentResult;

typedef struct {
    /* 1: rows [row_top, row_end) of `back` hold the scaled canvas.
       0: refused before anything was submitted.
       -1: submitted, but completion was not established. */
    int (*scale)(void *context, uint16_t *back,
                 const RenderCanvasDeferral *deferral,
                 unsigned row_top, unsigned row_end, uint64_t *elapsed_us);
    /* Whether an earlier failed submission may still be running. */
    bool (*busy)(void *context);
    /* Close shared GE admission for every other user. */
    void (*fail)(void *context, const char *reason);
    /* Free what scale() allocated; true when nothing is held afterwards. */
    bool (*release)(void *context);
    void *context;
} CanvasGeBackend;

typedef struct {
    uint32_t presents, ge_refusals, failures, foreign_refusals;
    uint32_t busy_refusals, materializations, materialize_failures;
    uint32_t scratch_releases;
    uint32_t verify_frames, verify_bad_frames, verify_unavailable;
    uint64_t verify_pixels, verify_mismatches;
    uint64_t ge_us, ge_max_us;
} CanvasGePresenterStats;

typedef struct {
    CanvasGeBackend backend;
    bool enabled;
    bool failed;
    bool verify;
    bool outstanding;
    /* The backend may hold scratch since a scale() made on scratch_page. */
    bool scratch_held;
    uint64_t page, scratch_page;
    CanvasGePresenterStats stats;
} CanvasGePresenter;

void canvas_ge_presenter_init(CanvasGePresenter *presenter,
                              const CanvasGeBackend *backend);
/* Turns deferral on or off for the process (tile_cache_set_canvas_defer).
   A latched failure keeps it off. `verify` (diagnostics) materializes the
   CPU frame after each GE publication and compares the published rows. */
void canvas_ge_presenter_enable(CanvasGePresenter *presenter, bool enabled,
                                bool verify);
/* The page now presented (browser_engine_page_generation). */
void canvas_ge_presenter_set_page(CanvasGePresenter *presenter,
                                  uint64_t page);
CanvasGePresentResult canvas_ge_presenter_publish(
    CanvasGePresenter *presenter, TileCache *cache, const uint16_t *frame,
    uint16_t *back, size_t back_stride, unsigned width,
    unsigned row_top, unsigned row_end, bool owner_thread);

#endif
