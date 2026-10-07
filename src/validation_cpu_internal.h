#ifndef TILEFINCH_VALIDATION_CPU_INTERNAL_H
#define TILEFINCH_VALIDATION_CPU_INTERNAL_H

#include "tilefinch/validation_cpu.h"
#include "validation_phase_internal.h"

/* Batch-only CPU scopes. No allocation, output, or platform clock in this
   reducer; the caller supplies brackets from one owner-thread CPU clock. */
#define VALIDATION_CPU_DEPTH 12u
typedef struct {
    bool enabled;
    uint32_t depth;
    ScriptRuntimeCpuMetrics metrics;
    ValidationPhaseScope stack[VALIDATION_CPU_DEPTH];
} ValidationCpuCapture;

static inline void validation_cpu_error(ValidationCpuCapture *c)
{
    if (c->metrics.errors != UINT32_MAX) c->metrics.errors++;
}

static inline bool validation_cpu_clock(ValidationCpuCapture *c, ValidationPhaseClock t)
{
    if (!t.valid || t.wall_end_us < t.wall_begin_us) {
        validation_cpu_error(c); return false;
    }
    uint64_t span = t.wall_end_us - t.wall_begin_us;
    if (span > UINT64_MAX - c->metrics.observer_us) {
        validation_cpu_error(c); return false;
    }
    c->metrics.observer_us += span;
    if (span > c->metrics.maximum_query_us) c->metrics.maximum_query_us = span;
    return true;
}

static inline void validation_cpu_enter(ValidationCpuCapture *c, uint32_t phase,
                                 ValidationPhaseClock t)
{
    if (!c->enabled) return;
    if (phase >= SCRIPT_RUNTIME_CPU_PHASES || c->depth >= VALIDATION_CPU_DEPTH
        || !validation_cpu_clock(c, t)) {
        validation_cpu_error(c); c->depth = 0; return;
    }
    c->stack[c->depth++] = (ValidationPhaseScope){.begin=t, .phase=phase};
}

static inline void validation_cpu_leave(ValidationCpuCapture *c, uint32_t phase,
                                 ValidationPhaseClock t)
{
    if (!c->enabled) return;
    if (c->depth == 0u || c->stack[c->depth - 1u].phase != phase
        || !validation_cpu_clock(c, t)) {
        /* A bad diagnostic clock or malformed scope must not leave an
           unwind loop stuck. Invalidate this interval and abandon nesting. */
        validation_cpu_error(c); c->depth = 0; return;
    }
    ValidationPhaseScope *scope = &c->stack[--c->depth];
    if (t.cpu_us < scope->begin.cpu_us
        || t.wall_begin_us < scope->begin.wall_end_us) {
        validation_cpu_error(c); return;
    }
    uint64_t cpu = t.cpu_us - scope->begin.cpu_us;
    if (scope->children_cpu_us > cpu
        || cpu > UINT64_MAX - c->metrics.inclusive_cpu_us[phase]
        || cpu - scope->children_cpu_us > UINT64_MAX - c->metrics.exclusive_cpu_us[phase]
        || c->metrics.calls[phase] == UINT64_MAX) {
        validation_cpu_error(c); return;
    }
    c->metrics.inclusive_cpu_us[phase] += cpu;
    c->metrics.exclusive_cpu_us[phase] += cpu - scope->children_cpu_us;
    c->metrics.calls[phase]++;
    if (c->depth != 0u) {
        ValidationPhaseScope *parent = &c->stack[c->depth - 1u];
        if (cpu > UINT64_MAX - parent->children_cpu_us) validation_cpu_error(c);
        else parent->children_cpu_us += cpu;
    }
}
#endif
