#include "psp_hls_gzip.h"

#include <limits.h>
#include <string.h>

static voidpf hls_gzip_alloc(voidpf opaque, uInt items, uInt size)
{
    Budget *budget = opaque;
    if (budget == NULL || (size != 0u && items > UINT_MAX / size)) return NULL;
    return budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, items, size);
}

static void hls_gzip_free(voidpf opaque, voidpf address)
{
    if (opaque != NULL) budget_free(opaque, address);
}

bool psp_hls_gzip_begin(PspHlsGzip *gzip, Budget *budget)
{
    if (gzip == NULL || budget == NULL) return false;
    psp_hls_gzip_reset(gzip);
    gzip->budget = budget;
    gzip->stream.zalloc = hls_gzip_alloc;
    gzip->stream.zfree = hls_gzip_free;
    gzip->stream.opaque = budget;
    if (inflateInit2(&gzip->stream, MAX_WBITS + 16) != Z_OK) {
        memset(gzip, 0, sizeof(*gzip));
        return false;
    }
    gzip->initialized = true;
    return true;
}

PspHlsGzipStatus psp_hls_gzip_pump(
    PspHlsGzip *gzip, const unsigned char *input, size_t input_length,
    unsigned char *output, size_t output_capacity,
    size_t *consumed, size_t *produced, int *zlib_status)
{
    if (consumed != NULL) *consumed = 0;
    if (produced != NULL) *produced = 0;
    if (zlib_status != NULL) *zlib_status = Z_STREAM_ERROR;
    if (gzip == NULL || !gzip->initialized || gzip->finished
        || input == NULL || output == NULL || input_length == 0
        || output_capacity == 0 || input_length > UINT_MAX
        || output_capacity > UINT_MAX || consumed == NULL
        || produced == NULL) return PSP_HLS_GZIP_MALFORMED;

    gzip->stream.next_in = (Bytef *) input;
    gzip->stream.avail_in = (uInt) input_length;
    gzip->stream.next_out = output;
    gzip->stream.avail_out = (uInt) output_capacity;
    int status = inflate(&gzip->stream, Z_NO_FLUSH);
    *consumed = input_length - gzip->stream.avail_in;
    *produced = output_capacity - gzip->stream.avail_out;
    if (zlib_status != NULL) *zlib_status = status;
    if (status == Z_STREAM_END) {
        gzip->finished = true;
        return PSP_HLS_GZIP_FINISHED;
    }
    if (status != Z_OK && status != Z_BUF_ERROR)
        return status == Z_MEM_ERROR ? PSP_HLS_GZIP_REFUSED
                                     : PSP_HLS_GZIP_MALFORMED;
    if (*consumed == 0 && *produced == 0) return PSP_HLS_GZIP_STALLED;
    return PSP_HLS_GZIP_PROGRESS;
}

void psp_hls_gzip_reset(PspHlsGzip *gzip)
{
    if (gzip == NULL) return;
    if (gzip->initialized) (void) inflateEnd(&gzip->stream);
    memset(gzip, 0, sizeof(*gzip));
}
