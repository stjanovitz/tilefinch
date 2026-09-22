#ifndef TILEFINCH_PSP_INPUT_CADENCE_H
#define TILEFINCH_PSP_INPUT_CADENCE_H

#include <stdbool.h>
#include <stdint.h>

/* Validation-only callers: intentional idle/clamped samples end a segment,
   but must not erase accumulated measurements or pending presentation age. */
typedef struct {
    uint64_t previous_us, total_us, max_us;
    uint32_t intervals, segments;
    bool active;
} PspInputCadence;

static inline void psp_input_cadence_observe(
    PspInputCadence *cadence, bool moving, uint64_t now_us)
{
    if (!moving) {
        cadence->active = false;
        return;
    }
    if (cadence->active && now_us >= cadence->previous_us) {
        uint64_t interval = now_us - cadence->previous_us;
        cadence->total_us += interval;
        cadence->intervals++;
        if (interval > cadence->max_us) cadence->max_us = interval;
    } else {
        cadence->segments++;
    }
    cadence->previous_us = now_us;
    cadence->active = true;
}

#endif
