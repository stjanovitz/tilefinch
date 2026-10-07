#ifndef TILEFINCH_RUNTIME_CLOCK_H
#define TILEFINCH_RUNTIME_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* How far one runtime advance moves the page's timer clock.

   The PSP runs the scheduler on a virtual clock (it does not sample the
   wall clock): each advance moves setTimeout, setInterval and
   requestAnimationFrame deadlines by the milliseconds it is handed. The
   frame loop used to hand every advance a whole tick (16 ms), which is
   wall time only while frames are one vblank long; a turn started early
   (a page task already waiting, an unfinished raster job, cursor motion)
   would run the page's timers and animation frames faster than real time.

   An advance is now handed the wall time since the previous one, carrying
   sub-millisecond remainders, and at most one tick: a long frame still
   moves the clock by one tick, as before (page timers do not catch up on a
   busy frame's time). The first advance gets a whole tick. */
typedef struct {
    uint64_t last_us;
    uint32_t carry_us;
    bool started;
} TilefinchRuntimeClock;

static inline unsigned tilefinch_runtime_clock_step(
    TilefinchRuntimeClock *clock, uint64_t now_us, unsigned tick_ms)
{
    if (clock == 0) return tick_ms;
    uint64_t last = clock->last_us;
    bool started = clock->started;
    clock->last_us = now_us;
    clock->started = true;
    uint32_t tick_us = (uint32_t) tick_ms * 1000u;
    if (!started || now_us < last || now_us - last >= tick_us) {
        clock->carry_us = 0;
        return tick_ms;
    }
    /* Below one tick: 32-bit arithmetic, no software 64-bit division. */
    uint32_t elapsed_us = (uint32_t) (now_us - last) + clock->carry_us;
    if (elapsed_us >= tick_us) {
        clock->carry_us = 0;
        return tick_ms;
    }
    clock->carry_us = elapsed_us % 1000u;
    return elapsed_us / 1000u;
}

/* Predict the short wait needed for a virtual-clock frame timer, without
   consuming elapsed time. Refuse deadlines beyond one admitted tick: this
   is not permission to catch up timers or spin through long delays. */
static inline bool tilefinch_runtime_clock_frame_wait(
    const TilefinchRuntimeClock *clock, uint64_t now_us, unsigned tick_ms,
    unsigned required_ms, uint32_t *wait_us)
{
    if (clock == 0 || wait_us == 0 || required_ms > tick_ms) return false;
    TilefinchRuntimeClock projected = *clock;
    unsigned step_ms = tilefinch_runtime_clock_step(&projected, now_us, tick_ms);
    if (step_ms >= required_ms) *wait_us = 0;
    else *wait_us = (required_ms - step_ms) * 1000u - projected.carry_us;
    return true;
}

#endif
