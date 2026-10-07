#ifndef TILEFINCH_IMAGE_RETARGET_H
#define TILEFINCH_IMAGE_RETARGET_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/layout.h"
#include "tilefinch/resources.h"
#include "tilefinch/session.h"

/* Display retargeting: once a committed layout says how large each decoded
   raster is painted, keep only what is painted.

   - A surface painted at half its decoded area or less is reduced to the
     painted size by point sampling the decoded copy (image_point_sample_rgba,
     shared with the decoders' own reductions). Aspect is preserved, so cover,
     contain and sprite geometry are unchanged.
   - A reduced surface that is later painted larger (relayout at another
     viewport, zoom) is decoded again from its retained encoded bytes at the
     size it was first decoded at, and is never reduced again.
   - A surface no command paints is released down to its encoded bytes; the
     paint path decodes encoded rasters at their painted size, and the next
     step after it becomes painted decodes it at that size.

   - SVG rasters that keep their markup (IMAGE_RETARGET_VECTOR) take part
     the same way, except that a reduction rasterizes the markup at the
     painted size instead of sampling, and a later larger paint rasterizes it
     at that size (at least doubling, never past the first raster's size)
     rather than at its first size.

   Only surfaces that keep their encoded bytes take part (canvas surfaces,
   adopted session-cache leases, SVG rasters whose markup outweighs them and
   images page script has read are left alone), so every reduction can be
   undone. Session-cache copies of a replaced surface are dropped rather
   than replaced: a later page decodes again at its own size instead of
   adopting a smaller one. */

typedef enum {
    /* Nothing to do for this layout. */
    IMAGE_RETARGET_IDLE = 0,
    /* One surface changed; more may follow on the next step. */
    IMAGE_RETARGET_CHANGED,
    /* A decode was refused for now (busy decoder or memory); retry later. */
    IMAGE_RETARGET_RETRY,
    /* The layout still references a retired resource table; retry after it
       is rebound. */
    IMAGE_RETARGET_STALE
} ImageRetargetOutcome;

/* One bounded step: scans once per layout generation and changes at most
   one surface. The caller supplies a generation that changes on relayout. */
ImageRetargetOutcome images_retarget_display_step(
    ImageResources *images, const LayoutDocument *layout,
    BrowserSession *session, size_t generation);
/* A retarget-only publication did not change command geometry. */
void images_retarget_plan_rebase(ImageResources *images, size_t generation);
void images_retarget_plan_discard(ImageResources *images);

/* Page script is about to read the pixels of images->items[index]: decode it
   back to full resolution if it was reduced or released, and pin it there.
   Returns false only when the pixels could not be restored. */
bool images_retarget_pin_full(ImageResources *images, size_t index,
                              BrowserSession *session);

#endif
