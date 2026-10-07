#ifndef TILEFINCH_PSP_CANVAS_GE_H
#define TILEFINCH_PSP_CANVAS_GE_H

/*
 * GE-direct presenter backend: publish a deferred full-viewport
 * WebGL canvas (render.h RenderCanvasDeferral) by letting the GE scale the
 * authoritative EDRAM surface straight into the RGB565 display back buffer.
 *
 * Geometry is the exact CPU kernel's: each source row owns the integer
 * destination rows floor-mapped to it, and the horizontal 2:3 nearest
 * mapping is reproduced with a quarter-texel bias. Rows are cut into
 * 32-texel column strips (strip-major) so the texture cache walks a narrow
 * column instead of whole 1,280-byte rows; the boot-time probe measured
 * 0.65 ms for 320x180 -> 480x272 against 4.3 ms for whole-row sprites, with
 * all 130,560 pixels equal to the CPU oracle.
 *
 * Ownership is synchronous. The caller is the browser's owner thread; the
 * back buffer is written back/invalidated from the CPU cache before the GE
 * may write it, and the CPU touches it again only after completion has been
 * observed within a bounded wait. The RAM frame and the WebGL surface are
 * never written.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/canvas_ge_presenter.h"
#include "tilefinch/render.h"

/* CanvasGeBackend implementation (canvas_ge_presenter.h). scale() returns
   1 when rows [row_top, row_end) of `back` hold the canvas, 0 when refused
   before submission (the caller falls back to the CPU path), and -1 when a
   submission's completion could not be established within the bounded wait
   (busy() then reports whether it is still running). */
int psp_canvas_ge_scale(void *context, uint16_t *back,
                        const RenderCanvasDeferral *deferral,
                        unsigned row_top, unsigned row_end,
                        uint64_t *elapsed_us);
bool psp_canvas_ge_busy(void *context);
void psp_canvas_ge_fail(void *context, const char *reason);

#endif
