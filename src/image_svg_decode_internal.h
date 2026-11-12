#ifndef TILEFINCH_IMAGE_SVG_DECODE_INTERNAL_H
#define TILEFINCH_IMAGE_SVG_DECODE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/budget.h"

/* Rasterizes SVG markup. On entry *width and *height are the raster size
   wanted (zero for the document's own size, rounded up); the picture is
   scaled uniformly to fit it. On success they hold the raster's size.
   Returns NULL on failure, including while another rasterization is in
   progress (image_svg_decode_busy). */
bool image_svg_decode_busy(void);
typedef bool (*ImageSvgOversizeTarget)(void *opaque, int source_width,
                                      int source_height, int *width,
                                      int *height);
/* Only consult the target when the intrinsic raster exceeds the quota.
   Successful ordinary decodes are unchanged; intrinsic dimensions remain
   separate from the returned pixels. Refused allocations are never retried.
   On a NULL return, *limited (when not NULL) tells why: true when the
   decode was refused by this engine's limits rather than by the markup,
   that is for memory (a Budget refusal while parsing or rasterizing), the
   decoded-bytes quota, a declined or missing oversize target, an
   intrinsic size past 32767 px, nesting past the parser's depth limits or
   re-entry; false for markup no decoder
   takes, which fails the same way every time. */
unsigned char *image_svg_decode_bounded(
    const void *data, size_t length, Budget *budget,
    size_t maximum_decoded_bytes, int *width, int *height,
    ImageSvgOversizeTarget target, void *opaque,
    int *source_width, int *source_height, bool *limited);
/* As image_svg_decode_bounded, and once the markup is parsed `defer` (when
   not NULL) sees its intrinsic size first: returning true skips the raster.
   The call then returns NULL with *deferred set, *limited false and the
   intrinsic size in *width, *height, *source_width and *source_height, so
   a caller can keep the markup and rasterize it later without parsing it
   twice now. With `kept` not NULL, a deferred document's parse is also
   kept in *kept when it holds keep_limit bytes or fewer
   (image_svg_parse_bytes), so the later raster need not parse it again;
   it holds budget memory until image_svg_parse_rasterize or
   image_svg_parse_release. */
typedef bool (*ImageSvgDefer)(void *opaque, int source_width,
                              int source_height);
typedef struct ImageSvgParse ImageSvgParse;
unsigned char *image_svg_decode_deferrable(
    const void *data, size_t length, Budget *budget,
    size_t maximum_decoded_bytes, int *width, int *height,
    ImageSvgOversizeTarget target, void *opaque,
    ImageSvgDefer defer, void *defer_opaque,
    int *source_width, int *source_height, bool *limited, bool *deferred,
    size_t keep_limit, ImageSvgParse **kept);
/* Whether `parse` was made from exactly these markup bytes. */
bool image_svg_parse_matches(const ImageSvgParse *parse, const void *data,
                             size_t length);
/* Budget bytes a kept parse holds (its markup copy, document and clip
   state). */
size_t image_svg_parse_bytes(const ImageSvgParse *parse);
/* Rasterizes a kept parse at *width x *height (scaled uniformly to fit,
   as image_svg_decode does for that size), within maximum_decoded_bytes,
   and releases the parse, whatever the outcome. NULL on failure,
   including while another rasterization is in progress. */
unsigned char *image_svg_parse_rasterize(ImageSvgParse *parse,
                                         size_t maximum_decoded_bytes,
                                         int *width, int *height);
void image_svg_parse_release(ImageSvgParse *parse);
/* Documents parsed by this decoder since start (a work counter). */
size_t image_svg_parse_count(void);
unsigned char *image_svg_decode(const void *data, size_t length,
                                Budget *budget,
                                size_t maximum_decoded_bytes,
                                int *width, int *height);

#endif
