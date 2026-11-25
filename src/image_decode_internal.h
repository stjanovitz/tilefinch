#ifndef TILEFINCH_IMAGE_DECODE_INTERNAL_H
#define TILEFINCH_IMAGE_DECODE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/resources.h"

typedef struct {
    ImageDecodeStatus status;
    unsigned char *pixels;
    size_t pixel_bytes;
    size_t working_bytes;
    int source_width;
    int source_height;
} ImageDecodeWorkerResult;

typedef enum {
    IMAGE_DECODE_SUBMIT_REJECTED = -1,
    IMAGE_DECODE_SUBMIT_BUSY = 0,
    IMAGE_DECODE_SUBMIT_ACCEPTED = 1
} ImageDecodeSubmitResult;

typedef enum {
    IMAGE_DECODE_PROBE_UNSUPPORTED = 0,
    IMAGE_DECODE_PROBE_SUPPORTED,
    IMAGE_DECODE_PROBE_BUSY
} ImageDecodeProbeResult;

bool image_decode_busy(void);
/* Lossless one-byte samples for sequential grayscale JPEGs, used only by
   the renderer's pressure fallback. Not an RGBA surface. */
ImageDecodeStatus image_resource_decode_gray_checked(
    const ImageResource *image, Budget *budget, unsigned char **decoded);
/* Peak bytes the last image_resource_decode_checked() held at once (its
   allocations, libwebp's, and the output). */
size_t image_decode_last_peak_bytes(void);
/* Point sampling, shared by every reduction (after a full-size decode, the
   streaming row sampler and display retargeting) so their pixels stay
   identical: target index `at` of `target` takes source index
   at * source / target. */
static inline int image_point_sample_index(int at, int source, int target)
{
    return (int) ((int64_t) at * source / target);
}
/* Point-samples a source_width x source_height RGBA surface into a
   target_width x target_height one. */
void image_point_sample_rgba(const unsigned char *source, int source_width,
                             int source_height, unsigned char *target,
                             int target_width, int target_height);
ImageDecodeProbeResult image_decode_probe_info(
    Budget *budget, const unsigned char *encoded, size_t encoded_length,
    int *width, int *height, int *components, bool *is_webp);
/* Free Budget a worker decode leaves beside its arena. A submit that
   would leave less is refused, with the arena plus this reserve as the
   refused charge. */
#define IMAGE_DECODE_WORKER_RESERVE (2u * 1024u * 1024u)
/* A JPEG decode at *width x *height of `image` (encoded, source size set)
   that runs a full-scale IDCT, being above half its source in a dimension,
   and was refused for memory: the size of its first reduced decode, at half
   and then a quarter of the source, that fits. A worker decode fits when its
   arena leaves IMAGE_DECODE_WORKER_RESERVE free; any other when it fits
   while it runs and the pixels it keeps leave that reserve free. A size
   that would keep more than `kept_limit` is skipped. *needed is the Budget
   the decode as asked takes while it runs (0 when it is not a JPEG whose
   frame header can be read). */
bool image_decode_reduced_fallback(const ImageResource *image,
                                   const Budget *budget, bool worker,
                                   size_t kept_limit, int *width, int *height,
                                   size_t *needed);
ImageDecodeSubmitResult image_decode_worker_submit(
    const ImageResource *resource, Budget *budget, uint32_t *token,
    size_t *refused_charge);
bool image_decode_worker_collect(uint32_t token,
                                 ImageDecodeWorkerResult *result);
void image_decode_worker_abandon(uint32_t token);

#endif
