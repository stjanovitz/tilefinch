#include "audio_gate.h"

#define MIN_UTTERANCE_MILLISECONDS 200U
#define MIN_CENTERED_RMS 64U
#define MAX_CLIPPED_PERCENT 40U
#define MAX_ZERO_CROSSING_PERCENT 40U

CaptureStatus inspect_and_condition_capture(
    int16_t *samples,
    size_t count,
    unsigned sample_rate
)
{
    int64_t sum = 0;
    uint64_t centered_squares = 0;
    size_t clipped = 0;
    size_t zero_crossings = 0;
    size_t index;
    int32_t mean;

    if (!samples || !count)
        return CAPTURE_EMPTY;
    if (!sample_rate || count * 1000U < sample_rate * MIN_UTTERANCE_MILLISECONDS)
        return CAPTURE_TOO_SHORT;
    for (index = 0; index < count; ++index) {
        int32_t value = samples[index];
        sum += value;
        if (value >= 32760 || value <= -32760)
            ++clipped;
    }
    mean = (int32_t)(sum / (int64_t)count);
    for (index = 0; index < count; ++index) {
        int32_t centered = (int32_t)samples[index] - mean;
        centered_squares += (uint64_t)((int64_t)centered * centered);
        if (
            index > 0
            && ((centered >= 0) != ((int32_t)samples[index - 1] - mean >= 0))
        )
            ++zero_crossings;
    }
    if (centered_squares < (uint64_t)count * MIN_CENTERED_RMS * MIN_CENTERED_RMS)
        return CAPTURE_TOO_QUIET;
    if (clipped * 100U > count * MAX_CLIPPED_PERCENT)
        return CAPTURE_TOO_CLIPPED;
    if (
        count > 1
        && zero_crossings * 100U > (count - 1U) * MAX_ZERO_CROSSING_PERCENT
    )
        return CAPTURE_TOO_NOISY;
    for (index = 0; index < count; ++index) {
        int32_t centered = (int32_t)samples[index] - mean;
        if (centered > 32767)
            centered = 32767;
        else if (centered < -32768)
            centered = -32768;
        samples[index] = (int16_t)centered;
    }
    return CAPTURE_OK;
}

const char *capture_status_name(CaptureStatus status)
{
    switch (status) {
    case CAPTURE_OK:
        return "ok";
    case CAPTURE_EMPTY:
        return "empty";
    case CAPTURE_TOO_SHORT:
        return "too_short";
    case CAPTURE_TOO_QUIET:
        return "too_quiet";
    case CAPTURE_TOO_CLIPPED:
        return "too_clipped";
    case CAPTURE_TOO_NOISY:
        return "too_noisy";
    default:
        return "unknown";
    }
}
