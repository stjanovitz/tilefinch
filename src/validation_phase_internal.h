/* Validation-only hierarchical accounting. The caller supplies bracketed
   clocks: this module performs no syscalls, allocations or diagnostic I/O. */
#ifndef TILEFINCH_VALIDATION_PHASE_INTERNAL_H
#define TILEFINCH_VALIDATION_PHASE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Reserve disjoint IDs for selectively detailed subsystems; reusing HUD or
   audio IDs for AI stages would silently mix unlike work in one total. */
#define VALIDATION_PHASE_LIMIT 40u
#define VALIDATION_PHASE_DEPTH 12u
#define VALIDATION_PHASE_FRAME_LIMIT 512u
#define VALIDATION_PHASE_COUNTER_LIMIT 40u

typedef struct {
    uint64_t cpu_us, wall_begin_us, wall_end_us;
    bool valid;
} ValidationPhaseClock;

typedef struct {
    uint32_t cpu_us, exclusive_cpu_us, wall_us, calls;
} ValidationPhaseTotals;

typedef struct {
    uint32_t sequence, checksum_a, checksum_b;
    uint32_t cpu_us, wall_us, observer_us, uncertainty_us;
    uint64_t begin_us, end_us;
    ValidationPhaseTotals phases[VALIDATION_PHASE_LIMIT];
    uint32_t counters[VALIDATION_PHASE_COUNTER_LIMIT];
} ValidationPhaseFrame;

typedef struct {
    ValidationPhaseClock begin;
    uint64_t children_cpu_us;
    uint32_t phase;
} ValidationPhaseScope;

typedef struct ValidationPhaseCapture {
    uint32_t mode; /* 1 coarse clocks, 2 counters only, 3 detailed clocks */
    uint32_t count, depth, errors;
    bool active;
    ValidationPhaseClock begin;
    ValidationPhaseScope stack[VALIDATION_PHASE_DEPTH];
    ValidationPhaseFrame frames[VALIDATION_PHASE_FRAME_LIMIT];
} ValidationPhaseCapture;

static bool validation_phase_fail(ValidationPhaseCapture *capture)
{
    if (capture->errors != UINT32_MAX) capture->errors++;
    return false;
}

static bool validation_phase_add(ValidationPhaseCapture *capture,
    uint32_t *target, uint64_t amount)
{
    if (amount > UINT32_MAX - *target) return validation_phase_fail(capture);
    *target += (uint32_t) amount;
    return true;
}

static bool validation_phase_clock(ValidationPhaseCapture *capture,
    ValidationPhaseClock clock)
{
    if (!clock.valid || clock.wall_end_us < clock.wall_begin_us)
        return validation_phase_fail(capture);
    ValidationPhaseFrame *frame = &capture->frames[capture->count];
    uint64_t span = clock.wall_end_us - clock.wall_begin_us;
    if (span > frame->uncertainty_us) {
        if (span > UINT32_MAX) return validation_phase_fail(capture);
        frame->uncertainty_us = (uint32_t) span;
    }
    return validation_phase_add(capture, &frame->observer_us, span);
}

static bool validation_phase_delta(ValidationPhaseCapture *capture,
    ValidationPhaseClock begin, ValidationPhaseClock end,
    uint64_t *cpu, uint64_t *wall)
{
    if (!begin.valid || !end.valid || end.cpu_us < begin.cpu_us
        || end.wall_begin_us < begin.wall_end_us)
        return validation_phase_fail(capture);
    *cpu = end.cpu_us - begin.cpu_us;
    /* Midpoint is an estimate, not an exact time of the CPU read. Preserve
       the largest bracket width separately; never subtract observer time
       wholesale from owner or off-CPU time. */
    uint64_t a = begin.wall_begin_us
        + (begin.wall_end_us - begin.wall_begin_us) / 2u;
    uint64_t b = end.wall_begin_us
        + (end.wall_end_us - end.wall_begin_us) / 2u;
    *wall = b - a;
    return true;
}

static bool validation_phase_begin(ValidationPhaseCapture *capture,
    uint32_t sequence, ValidationPhaseClock clock)
{
    if (capture->active || capture->errors != 0u || capture->mode < 1u
        || capture->mode > 3u || capture->count >= VALIDATION_PHASE_FRAME_LIMIT)
        return validation_phase_fail(capture);
    ValidationPhaseFrame *frame = &capture->frames[capture->count];
    memset(frame, 0, sizeof(*frame));
    frame->sequence = sequence;
    capture->begin = clock;
    capture->depth = 0;
    capture->active = true;
    if (capture->mode != 2u) {
        frame->begin_us = clock.wall_begin_us;
        return validation_phase_clock(capture, clock);
    }
    return true;
}

static bool validation_phase_enter(ValidationPhaseCapture *capture,
    uint32_t phase, ValidationPhaseClock clock)
{
    if (!capture->active || capture->errors || phase >= VALIDATION_PHASE_LIMIT
        || capture->depth >= VALIDATION_PHASE_DEPTH)
        return validation_phase_fail(capture);
    ValidationPhaseScope *scope = &capture->stack[capture->depth++];
    *scope = (ValidationPhaseScope) {.begin = clock, .phase = phase};
    ValidationPhaseFrame *frame = &capture->frames[capture->count];
    if (!validation_phase_add(capture, &frame->phases[phase].calls, 1u))
        return false;
    return capture->mode != 3u || validation_phase_clock(capture, clock);
}

static bool validation_phase_leave(ValidationPhaseCapture *capture,
    uint32_t phase, ValidationPhaseClock clock)
{
    if (!capture->active || capture->errors || capture->depth == 0u
        || capture->stack[capture->depth - 1u].phase != phase)
        return validation_phase_fail(capture);
    ValidationPhaseScope *scope = &capture->stack[--capture->depth];
    if (capture->mode != 3u) return true;
    uint64_t cpu, wall;
    if (!validation_phase_clock(capture, clock)
        || !validation_phase_delta(capture, scope->begin, clock, &cpu, &wall))
        return false;
    if (scope->children_cpu_us > cpu) return validation_phase_fail(capture);
    ValidationPhaseTotals *totals =
        &capture->frames[capture->count].phases[phase];
    if (!validation_phase_add(capture, &totals->cpu_us, cpu)
        || !validation_phase_add(capture, &totals->exclusive_cpu_us,
                                 cpu - scope->children_cpu_us)
        || !validation_phase_add(capture, &totals->wall_us, wall))
        return false;
    if (capture->depth != 0u)
        capture->stack[capture->depth - 1u].children_cpu_us += cpu;
    return true;
}

static bool validation_phase_end(ValidationPhaseCapture *capture,
    ValidationPhaseClock clock, uint32_t checksum_a, uint32_t checksum_b,
    const uint32_t *counters, size_t count)
{
    if (!capture->active || capture->errors || capture->depth != 0u
        || count > VALIDATION_PHASE_COUNTER_LIMIT || (count && !counters))
        return validation_phase_fail(capture);
    ValidationPhaseFrame *frame = &capture->frames[capture->count];
    if (capture->mode != 2u) {
        uint64_t cpu, wall;
        if (!validation_phase_clock(capture, clock)
            || !validation_phase_delta(capture, capture->begin, clock, &cpu, &wall)
            || !validation_phase_add(capture, &frame->cpu_us, cpu)
            || !validation_phase_add(capture, &frame->wall_us, wall))
            return false;
        frame->end_us = clock.wall_end_us;
    }
    frame->checksum_a = checksum_a;
    frame->checksum_b = checksum_b;
    if (count) memcpy(frame->counters, counters, count * sizeof(*counters));
    capture->active = false;
    capture->count++;
    return true;
}

#endif
