#ifndef TILEFINCH_SWDEC_AUDIO_CLOCK_H
#define TILEFINCH_SWDEC_AUDIO_CLOCK_H

#include "swdec_publish.h"
#include <stddef.h>

/* A DAC observation published with its accepted-sample endpoint. Never
   extrapolate beyond that endpoint: an underrun must stop the clock, not
   silently turn it into a wall clock. Uses the same non-waiting publication
   protocol as the other audio-thread values. */
typedef struct {
    SwdecPublishedU64 end;
    atomic_uint stamp_low[2], stamp_high[2], pending_us[2];
} SwdecAudioClock;

static inline void swdec_audio_clock_init(SwdecAudioClock *clock)
{
    swdec_published_init(&clock->end, 0);
    for (unsigned i = 0; i < 2; i++) {
        atomic_init(&clock->stamp_low[i], 0);
        atomic_init(&clock->stamp_high[i], 0);
        atomic_init(&clock->pending_us[i], 0);
    }
}

static inline void swdec_audio_clock_write(SwdecAudioClock *clock,
    uint64_t end, uint64_t now, uint32_t pending_us)
{
    unsigned slot = swdec_published_begin(&clock->end);
    swdec_published_store(&clock->end, slot, end);
    atomic_store_explicit(&clock->stamp_low[slot], (unsigned) now,
                          memory_order_relaxed);
    atomic_store_explicit(&clock->stamp_high[slot], (unsigned) (now >> 32),
                          memory_order_relaxed);
    atomic_store_explicit(&clock->pending_us[slot], pending_us,
                          memory_order_relaxed);
    swdec_published_commit(&clock->end);
}

static inline uint64_t swdec_audio_clock_sample(const SwdecAudioClock *clock,
    uint64_t now, uint64_t fallback, uint64_t *accepted_end)
{
    if (accepted_end != NULL) *accepted_end = 0;
    for (unsigned attempt = 0; attempt < SWDEC_PUBLISH_READ_ATTEMPTS; attempt++) {
        unsigned before = atomic_load_explicit(&clock->end.sequence,
                                               memory_order_acquire);
        unsigned slot = before & 1u;
        uint64_t end = atomic_load_explicit(&clock->end.low[slot], memory_order_relaxed);
        end |= (uint64_t) atomic_load_explicit(&clock->end.high[slot], memory_order_relaxed) << 32;
        uint64_t stamp = atomic_load_explicit(&clock->stamp_low[slot], memory_order_relaxed);
        stamp |= (uint64_t) atomic_load_explicit(&clock->stamp_high[slot], memory_order_relaxed) << 32;
        uint32_t pending = atomic_load_explicit(&clock->pending_us[slot], memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (before != atomic_load_explicit(&clock->end.sequence, memory_order_relaxed))
            continue;
        uint64_t elapsed = now > stamp ? now - stamp : 0;
        uint64_t left = elapsed < pending ? pending - elapsed : 0;
        if (accepted_end != NULL) *accepted_end = end;
        return left < end ? end - left : 0;
    }
    return fallback;
}

static inline uint64_t swdec_audio_clock_read(const SwdecAudioClock *clock,
    uint64_t now, uint64_t fallback)
{
    return swdec_audio_clock_sample(clock, now, fallback, NULL);
}

/* Recheck when a retained picture could become due, rather than one refresh
   later. A deadline just beyond the accepted block still merits that recheck:
   the audio thread may publish its successor during the sleep. This hint
   never advances the clock or authorizes presentation; the ordinary take
   gate still requires observed audio progress. Even an already-due picture
   yields briefly. Zero requests the existing pacing fallback. */
static inline uint32_t swdec_audio_due_wait_us(uint64_t pts, uint64_t duration,
    uint64_t clock, uint64_t accepted_end, uint32_t maximum_us)
{
    if (maximum_us == 0 || accepted_end == 0 || clock > accepted_end) return 0;
    uint64_t half = duration / 2u;
    uint64_t due = pts > half ? pts - half : 0;
    uint64_t delay = due > clock ? due - clock : 0;
    if (delay < 1000u) delay = 1000u;
    if (delay > maximum_us) delay = maximum_us;
    return (uint32_t) delay;
}

/* Refused/unavailable firmware observations retain the established block
   clock. Clamp the first block after a seek to the new stream origin. */
static inline uint32_t swdec_audio_clock_pending(int samples, uint32_t rate,
    uint64_t elapsed_audio_us)
{
    if (samples < 0 || samples > 2048 || rate == 0) return 0;
    uint64_t pending = (uint64_t) (unsigned) samples * 1000000u / rate;
    return (uint32_t) (pending < elapsed_audio_us ? pending : elapsed_audio_us);
}

#endif
