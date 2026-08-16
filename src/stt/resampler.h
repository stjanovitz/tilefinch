#ifndef PSP_STT_RESAMPLER_H
#define PSP_STT_RESAMPLER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Convert mono signed-16-bit PCM from the PSP microphone's supported
 * 22.05 kHz rate to PocketSphinx's 16 kHz acoustic-model rate.
 *
 * The converter applies a 17-tap Q15 low-pass filter and then Q16 linear
 * interpolation.  It allocates no temporary buffer.
 */
size_t resample_22050_to_16000(
    const int16_t *input,
    size_t input_count,
    int16_t *output,
    size_t output_capacity
);

/*
 * Equivalent conversion performed in the input buffer.  Since this is a
 * downsample, output advances more slowly than unread input.  A small prefix
 * copy protects the only initial region where FIR look-behind can overlap
 * samples already replaced by output.
 */
size_t resample_22050_to_16000_inplace(
    int16_t *buffer,
    size_t input_count
);

#endif
