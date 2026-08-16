#ifndef PSPSTTLAB_AUDIO_GATE_H
#define PSPSTTLAB_AUDIO_GATE_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    CAPTURE_OK = 0,
    CAPTURE_EMPTY,
    CAPTURE_TOO_SHORT,
    CAPTURE_TOO_QUIET,
    CAPTURE_TOO_CLIPPED,
    CAPTURE_TOO_NOISY
} CaptureStatus;

CaptureStatus inspect_and_condition_capture(
    int16_t *samples,
    size_t count,
    unsigned sample_rate
);

const char *capture_status_name(CaptureStatus status);

#endif
