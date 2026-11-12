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

   - A large SVG raster (IMAGE_RETARGET_DISTANT_MINIMUM_BYTES or more at
     its first size) also depends on where the reader is, when the caller
     says (ImageRetargetReader): it is held only while one of its paints
     lies within `near` of the viewport. One whose every paint lies beyond
     `far` is released to its markup (IMAGE_RETARGET_DISTANT), and is
     rasterized again once a paint comes within `near`: at its last size
     when it had grown, at the painted size when that is half its first
     area or less (what a reduction would leave), otherwise at its first
     size. A paint is judged where it is shown: moved by the scroll
     containers around it, and limited to what their clips (widened by the
     distance) can show, so scrolling a box inside the page counts as the
     reader moving. Paints of a viewport-fixed or sticky layer are always
     near. A load may leave such a CSS background unrasterized for the
     layout that follows (images_retarget_decode_awaiting), which
     rasterizes it before it is first painted only when it is near.

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

/* A large SVG raster's first size (width x height x 4) at or above which
   it is held only near the reader. Smaller ones (icons, wordmarks) cost
   less to keep than to rasterize again. */
#define IMAGE_RETARGET_DISTANT_MINIMUM_BYTES (256u * 1024u)

/* The reader's viewport [top, bottom) in layout (CSS) coordinates and the
   distances below and above it within which a large SVG raster is
   rasterized (near) and beyond which it is released (far, at least near). */
typedef struct {
    int top;
    int bottom;
    int near;
    int far;
} ImageRetargetReader;

/* One bounded step: scans once per layout generation (and reader window)
   and changes at most one surface. The caller supplies a generation that
   changes on relayout, and the reader (NULL: no distance rule). */
ImageRetargetOutcome images_retarget_display_step(
    ImageResources *images, const LayoutDocument *layout,
    BrowserSession *session, size_t generation,
    const ImageRetargetReader *reader);
/* Whether the table holds a surface the distance rule can change: a large
   SVG raster, or one released to its markup. */
bool images_retarget_distance_candidates(const ImageResources *images);
/* Before a frame at `reader`: rasterize, at most `limit` of them, the large
   SVG rasters that are released and painted within reader->near of the
   viewport, so a scroll that reached one before idle work did pays one
   rasterization, not one per tile. Returns how many were rasterized. */
size_t images_retarget_restore_near(ImageResources *images,
                                    const LayoutDocument *layout,
                                    BrowserSession *session,
                                    const ImageRetargetReader *reader,
                                    size_t limit);
/* A retarget-only publication did not change command geometry. */
void images_retarget_plan_rebase(ImageResources *images, size_t generation);
void images_retarget_plan_discard(ImageResources *images);

/* Before `layout` is first painted: decode each surface a load left
   encoded for it (IMAGE_RETARGET_AWAITING_LAYOUT) at the size `layout`
   paints it, never above its decoded size. Unpainted ones stay released
   for a later step. A large SVG raster is rasterized at the size the load
   would have given it, and only when `reader` (NULL: always) finds a paint
   of it near; one painted only far away stays released for the display
   step (IMAGE_RETARGET_DISTANT). Returns how many were decoded. */
size_t images_retarget_decode_awaiting(ImageResources *images,
                                       const LayoutDocument *layout,
                                       BrowserSession *session,
                                       const ImageRetargetReader *reader);

/* A load that leaves a large SVG background for the layout that follows
   (IMAGE_RETARGET_AWAITING_LAYOUT) may keep the parse that measured it, so
   images_retarget_decode_awaiting rasterizes it without parsing it again;
   one judged far is released there instead, so far SVG rasters hold
   neither pixels nor parsed documents. The allowance is the bytes a new
   parse may hold (zero when the table keeps no more); a held parse is
   keyed by `identity`, the item's encoded bytes, and is released with the
   table, under pressure (images_reclaim_rebuildable_rasters) or once no
   item waiting for a layout has those bytes. */
size_t images_retarget_svg_parse_allowance(const ImageResources *images);
void images_retarget_hold_svg_parse(ImageResources *images,
                                    const void *identity,
                                    struct ImageSvgParse *parse);
void images_retarget_release_svg_parses(ImageResources *images);

/* At an owner rebuild boundary, release only unpinned, reproducible pixels.
   Encoded bytes, intrinsic dimensions and aliases remain valid; painting or
   a later retarget step can restore them. Never call during a paint. */
size_t images_reclaim_rebuildable_rasters(ImageResources *images,
                                         BrowserSession *session,
                                         size_t needed_bytes);

/* Page script is about to read the pixels of images->items[index]: decode it
   back to full resolution if it was reduced or released, and pin it there.
   Returns false only when the pixels could not be restored. */
bool images_retarget_pin_full(ImageResources *images, size_t index,
                              BrowserSession *session);
/* Whether images_retarget_pin_full(images, index, ...) would decode the
   surface again (changing the table's pixels and decoded-byte stats) rather
   than only mark it pinned. */
bool images_retarget_pin_full_restores(ImageResources *images, size_t index);

#endif
