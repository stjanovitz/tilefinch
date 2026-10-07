#include "tilefinch/canvas_ge_presenter.h"

#include <string.h>

void canvas_ge_presenter_init(CanvasGePresenter *presenter,
                              const CanvasGeBackend *backend)
{
    if (presenter == NULL) return;
    memset(presenter, 0, sizeof(*presenter));
    if (backend != NULL) presenter->backend = *backend;
}

void canvas_ge_presenter_enable(CanvasGePresenter *presenter, bool enabled,
                                bool verify)
{
    if (presenter == NULL) return;
    presenter->enabled = enabled && presenter->backend.scale != NULL;
    presenter->verify = verify;
    tile_cache_set_canvas_defer(presenter->enabled && !presenter->failed);
}

static void canvas_ge_presenter_verify(
    CanvasGePresenter *presenter, TileCache *cache, const uint16_t *frame,
    const uint16_t *back, size_t back_stride, unsigned width,
    unsigned row_top, unsigned row_end)
{
    CanvasGePresenterStats *stats = &presenter->stats;
    if (!tile_cache_canvas_materialize(cache)) {
        stats->verify_unavailable++;
        return;
    }
    uint32_t mismatched = 0;
    for (unsigned y = row_top; y < row_end; y++)
        for (unsigned x = 0; x < width; x++)
            if (back[(size_t) y * back_stride + x]
                != frame[(size_t) y * width + x]) mismatched++;
    stats->verify_frames++;
    stats->verify_pixels += (uint64_t) (row_end - row_top) * width;
    stats->verify_mismatches += mismatched;
    if (mismatched != 0) stats->verify_bad_frames++;
}

CanvasGePresentResult canvas_ge_presenter_publish(
    CanvasGePresenter *presenter, TileCache *cache, const uint16_t *frame,
    uint16_t *back, size_t back_stride, unsigned width,
    unsigned row_top, unsigned row_end, bool owner_thread)
{
    if (presenter == NULL) return CANVAS_GE_PRESENT_COPY;
    CanvasGePresenterStats *stats = &presenter->stats;
    /* A submission whose completion was never established may still be
       writing a back buffer: nothing is published until it is gone. */
    if (presenter->outstanding) {
        if (presenter->backend.busy != NULL
            && presenter->backend.busy(presenter->backend.context)) {
            stats->busy_refusals++;
            return CANVAS_GE_PRESENT_REFUSE;
        }
        presenter->outstanding = false;
    }
    const RenderCanvasDeferral *deferral =
        cache != NULL && frame != NULL && frame == cache->frame
            ? tile_cache_canvas_deferral(cache) : NULL;
    if (deferral == NULL) return CANVAS_GE_PRESENT_COPY;
    if (!owner_thread) {
        stats->foreign_refusals++;
        return CANVAS_GE_PRESENT_REFUSE;
    }
    if (presenter->enabled && !presenter->failed && back != NULL
        && row_top < row_end && presenter->backend.scale != NULL) {
        uint64_t elapsed_us = 0;
        int drawn = presenter->backend.scale(
            presenter->backend.context, back, deferral, row_top, row_end,
            &elapsed_us);
        if (drawn > 0) {
            stats->presents++;
            stats->ge_us += elapsed_us;
            if (elapsed_us > stats->ge_max_us) stats->ge_max_us = elapsed_us;
            if (presenter->verify)
                canvas_ge_presenter_verify(presenter, cache, frame, back,
                                           back_stride, width, row_top,
                                           row_end);
            return CANVAS_GE_PRESENT_PUBLISHED;
        }
        if (drawn < 0) {
            stats->failures++;
            presenter->failed = true;
            presenter->outstanding = true;
            tile_cache_set_canvas_defer(false);
            if (presenter->backend.fail != NULL)
                presenter->backend.fail(presenter->backend.context,
                                        "canvas-ge-submit");
            return CANVAS_GE_PRESENT_REFUSE;
        }
        stats->ge_refusals++;
    }
    /* Exact CPU conversion, then the ordinary copy. */
    if (!tile_cache_canvas_materialize(cache)) {
        stats->materialize_failures++;
        return CANVAS_GE_PRESENT_REFUSE;
    }
    stats->materializations++;
    return CANVAS_GE_PRESENT_COPY;
}
