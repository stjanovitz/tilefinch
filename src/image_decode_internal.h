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
ImageDecodeProbeResult image_decode_probe_info(
    Budget *budget, const unsigned char *encoded, size_t encoded_length,
    int *width, int *height, int *components, bool *is_webp);
ImageDecodeSubmitResult image_decode_worker_submit(
    const ImageResource *resource, Budget *budget, uint32_t *token);
bool image_decode_worker_collect(uint32_t token,
                                 ImageDecodeWorkerResult *result);
void image_decode_worker_abandon(uint32_t token);

#endif
