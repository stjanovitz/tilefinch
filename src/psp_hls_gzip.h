#ifndef TILEFINCH_PSP_HLS_GZIP_H
#define TILEFINCH_PSP_HLS_GZIP_H

#include <stdbool.h>
#include <stddef.h>

#include <zlib.h>

#include "tilefinch/budget.h"

typedef enum {
    PSP_HLS_GZIP_PROGRESS = 0,
    PSP_HLS_GZIP_FINISHED,
    PSP_HLS_GZIP_REFUSED,
    PSP_HLS_GZIP_MALFORMED,
    PSP_HLS_GZIP_STALLED
} PspHlsGzipStatus;

typedef struct {
    z_stream stream;
    Budget *budget;
    bool initialized;
    bool finished;
} PspHlsGzip;

bool psp_hls_gzip_begin(PspHlsGzip *gzip, Budget *budget);
PspHlsGzipStatus psp_hls_gzip_pump(
    PspHlsGzip *gzip, const unsigned char *input, size_t input_length,
    unsigned char *output, size_t output_capacity,
    size_t *consumed, size_t *produced, int *zlib_status);
void psp_hls_gzip_reset(PspHlsGzip *gzip);

#endif
