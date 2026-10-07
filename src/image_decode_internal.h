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
ImageDecodeSubmitResult image_decode_worker_submit(
    const ImageResource *resource, Budget *budget, uint32_t *token);
bool image_decode_worker_collect(uint32_t token,
                                 ImageDecodeWorkerResult *result);
void image_decode_worker_abandon(uint32_t token);

#endif
