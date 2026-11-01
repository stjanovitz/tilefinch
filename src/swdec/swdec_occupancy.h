#ifndef TILEFINCH_SWDEC_OCCUPANCY_H
#define TILEFINCH_SWDEC_OCCUPANCY_H

/*
 * Media Engine occupancy from random samples of its published phase: a
 * phase seen k times in n samples has share p = k / n and, the samples being
 * taken at random times, a 95 % interval of 1.96 * sqrt(p (1 - p) / n).
 * Both are returned in tenths of a percent.
 */
static inline unsigned swdec_occupancy_isqrt64(unsigned long long value)
{
    unsigned long long root = 0, bit = 1ull << 62;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (unsigned) root;
}

static inline void swdec_occupancy_share(unsigned seen, unsigned samples,
                                         unsigned *share_tenths,
                                         unsigned *interval_tenths)
{
    *share_tenths = 0;
    *interval_tenths = 0;
    if (samples == 0 || seen > samples) return;
    unsigned long long k = seen, n = samples;
    *share_tenths = (unsigned) (k * 1000u / n);
    /* variance of p in ppm^2: k (n - k) 10^12 / n^3 */
    unsigned long long variance = k * (n - k) * 1000000ull / n * 1000000ull
        / n / n;
    *interval_tenths = (unsigned) (
        (1960ull * swdec_occupancy_isqrt64(variance) + 500000ull)
        / 1000000ull);
}

#endif
