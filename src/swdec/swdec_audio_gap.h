#ifndef TILEFINCH_SWDEC_AUDIO_GAP_H
#define TILEFINCH_SWDEC_AUDIO_GAP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Validation measurement: holes in the sound between output calls.
 *
 * The output call blocks until the hardware can take the next block, so
 * while sound is continuous consecutive calls RETURN one block duration
 * apart. A return later than that left the hardware without a queued block
 * for the excess: an audible hole, whether the block that followed was
 * sound or underrun silence. Each interval between consecutive returns
 * while playing is measured; the excess over one block is histogrammed and
 * counted as a gap above the threshold.
 */
#define SWDEC_AUDIO_GAP_THRESHOLD_US 5000u
#define SWDEC_AUDIO_GAP_BUCKETS 6u

/* Bucket upper bounds in microseconds of excess; the last is open. */
static const uint32_t swdec_audio_gap_bucket_us[SWDEC_AUDIO_GAP_BUCKETS - 1u] = {
    2000u, 5000u, 10000u, 23000u, 46000u
};

typedef struct {
    uint64_t last_return_us;  /* 0 = no baseline (start, pause, seek) */
    uint32_t intervals;
    uint32_t gaps;            /* excess above the threshold */
    uint64_t gap_total_us;    /* summed excess of those gaps */
    uint32_t max_excess_us;
    uint32_t histogram[SWDEC_AUDIO_GAP_BUCKETS];
} SwdecAudioGapStats;

/* Forget the baseline: the next return starts a new run (output paused,
   buffering, or a seek; those silences are deliberate). */
static inline void swdec_audio_gap_break(SwdecAudioGapStats *stats)
{
    stats->last_return_us = 0;
}

/* An output call returned at `now_us` while playing. */
static inline void swdec_audio_gap_note_return(
    SwdecAudioGapStats *stats, uint64_t now_us, uint32_t block_us)
{
    if (now_us == 0) now_us = 1;
    if (stats->last_return_us != 0 && now_us > stats->last_return_us) {
        uint64_t interval = now_us - stats->last_return_us;
        uint64_t excess64 = interval > block_us ? interval - block_us : 0;
        uint32_t excess = excess64 > UINT32_MAX ? UINT32_MAX
                                                : (uint32_t) excess64;
        unsigned bucket = 0;
        while (bucket < SWDEC_AUDIO_GAP_BUCKETS - 1u
               && excess >= swdec_audio_gap_bucket_us[bucket]) bucket++;
        stats->histogram[bucket]++;
        stats->intervals++;
        if (excess > stats->max_excess_us) stats->max_excess_us = excess;
        if (excess > SWDEC_AUDIO_GAP_THRESHOLD_US) {
            stats->gaps++;
            stats->gap_total_us += excess;
        }
    }
    stats->last_return_us = now_us;
}

#endif
