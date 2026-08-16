#include "resampler.h"

static const int16_t lowpass_q15[17] = {
    -68, 167, -95, -563, 1323, -319, -3705, 8932, 21424,
    8932, -3705, -319, 1323, -563, -95, 167, -68
};

static int16_t clamp16(int32_t value)
{
    if (value > 32767)
        return 32767;
    if (value < -32768)
        return -32768;
    return (int16_t)value;
}

static int16_t filter_at(
    const int16_t *input,
    size_t input_count,
    size_t index
)
{
    int64_t accum = 0;
    int tap;

    for (tap = 0; tap < 17; ++tap) {
        int source = (int)index + tap - 8;
        int16_t sample;
        if (source < 0)
            sample = input[0];
        else if ((size_t)source >= input_count)
            sample = input[input_count - 1];
        else
            sample = input[source];
        accum += (int32_t)sample * lowpass_q15[tap];
    }
    return clamp16((int32_t)((accum + (1 << 14)) >> 15));
}

static int16_t filter_at_inplace(
    const int16_t *input,
    size_t input_count,
    const int16_t *prefix,
    size_t prefix_count,
    size_t index
)
{
    int64_t accum = 0;
    int tap;

    for (tap = 0; tap < 17; ++tap) {
        int source = (int)index + tap - 8;
        int16_t sample;
        if (source < 0)
            sample = prefix[0];
        else if ((size_t)source >= input_count)
            sample = input[input_count - 1];
        else if ((size_t)source < prefix_count)
            sample = prefix[source];
        else
            sample = input[source];
        accum += (int32_t)sample * lowpass_q15[tap];
    }
    return clamp16((int32_t)((accum + (1 << 14)) >> 15));
}

size_t resample_22050_to_16000(
    const int16_t *input,
    size_t input_count,
    int16_t *output,
    size_t output_capacity
)
{
    const uint32_t step_q16 = (22050U << 16) / 16000U;
    size_t output_count;
    size_t i;
    uint64_t phase = 0;

    if (!input || !output || input_count == 0 || output_capacity == 0)
        return 0;
    output_count = (input_count * 16000U) / 22050U;
    if (output_count > output_capacity)
        output_count = output_capacity;

    for (i = 0; i < output_count; ++i) {
        size_t source = phase >> 16;
        uint32_t fraction = (uint32_t)(phase & 0xffff);
        int32_t first = filter_at(input, input_count, source);
        int32_t second = source + 1 < input_count
            ? filter_at(input, input_count, source + 1)
            : first;
        int32_t value = (
            first * (int32_t)(65536U - fraction)
            + second * (int32_t)fraction
        ) >> 16;
        output[i] = clamp16(value);
        phase += step_q16;
    }
    return output_count;
}

size_t resample_22050_to_16000_inplace(
    int16_t *buffer,
    size_t input_count
)
{
    const uint32_t step_q16 = (22050U << 16) / 16000U;
    int16_t prefix[64];
    size_t prefix_count;
    size_t output_count;
    size_t i;
    uint64_t phase = 0;

    if (!buffer || input_count == 0)
        return 0;
    prefix_count = input_count < 64 ? input_count : 64;
    for (i = 0; i < prefix_count; ++i)
        prefix[i] = buffer[i];
    output_count = (input_count * 16000U) / 22050U;

    for (i = 0; i < output_count; ++i) {
        size_t source = phase >> 16;
        uint32_t fraction = (uint32_t)(phase & 0xffff);
        int32_t first = filter_at_inplace(
            buffer,
            input_count,
            prefix,
            prefix_count,
            source
        );
        int32_t second = source + 1 < input_count
            ? filter_at_inplace(
                buffer,
                input_count,
                prefix,
                prefix_count,
                source + 1
            )
            : first;
        int32_t value = (
            first * (int32_t)(65536U - fraction)
            + second * (int32_t)fraction
        ) >> 16;
        buffer[i] = clamp16(value);
        phase += step_q16;
    }
    return output_count;
}
