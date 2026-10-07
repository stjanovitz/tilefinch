#ifndef TILEFINCH_PSP_LOG_CAPTURE_H
#define TILEFINCH_PSP_LOG_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Validation-only staging. Keep complete records or count their refusal;
   overflowing a timed capture must never initiate I/O in that window. */
#define PSP_LOG_CAPTURE_BYTES (32u * 1024u)
typedef struct {
    unsigned char bytes[PSP_LOG_CAPTURE_BYTES];
    size_t used;
    size_t dropped;
} PspLogCapture;

static inline bool psp_log_capture_append(
    PspLogCapture *capture, const void *bytes, size_t length)
{
    if (length > sizeof(capture->bytes) - capture->used) {
        if (capture->dropped != SIZE_MAX) capture->dropped++;
        return false;
    }
    memcpy(capture->bytes + capture->used, bytes, length);
    capture->used += length;
    return true;
}

#endif
