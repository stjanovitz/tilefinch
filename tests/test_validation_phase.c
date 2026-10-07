#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "../src/validation_phase_internal.h"
#include "../src/validation_cpu_internal.h"

static ValidationPhaseCapture capture;
static ValidationPhaseClock clock_at(uint64_t cpu, uint64_t wall)
{
    return (ValidationPhaseClock) {cpu, wall, wall + 2u, true};
}

int main(void)
{
    ValidationCpuCapture cpu = {.enabled=true};
    validation_cpu_enter(&cpu, SCRIPT_RUNTIME_CPU_ADVANCE, clock_at(0, 0));
    validation_cpu_enter(&cpu, SCRIPT_RUNTIME_CPU_PREPARATION, clock_at(5, 5));
    validation_cpu_leave(&cpu, SCRIPT_RUNTIME_CPU_PREPARATION, clock_at(15, 15));
    validation_cpu_enter(&cpu, SCRIPT_RUNTIME_CPU_CALLBACK, clock_at(20, 20));
    validation_cpu_leave(&cpu, SCRIPT_RUNTIME_CPU_CALLBACK, clock_at(50, 50));
    validation_cpu_enter(&cpu, SCRIPT_RUNTIME_CPU_JOBS, clock_at(55, 55));
    validation_cpu_enter(&cpu, SCRIPT_RUNTIME_CPU_WEBGL, clock_at(60, 60));
    validation_cpu_leave(&cpu, SCRIPT_RUNTIME_CPU_WEBGL, clock_at(80, 80));
    validation_cpu_leave(&cpu, SCRIPT_RUNTIME_CPU_JOBS, clock_at(90, 90));
    validation_cpu_enter(&cpu, SCRIPT_RUNTIME_CPU_REFRESH, clock_at(95, 95));
    validation_cpu_leave(&cpu, SCRIPT_RUNTIME_CPU_REFRESH, clock_at(105, 105));
    validation_cpu_leave(&cpu, SCRIPT_RUNTIME_CPU_ADVANCE, clock_at(110, 110));
    uint64_t reconciled = 0;
    for (unsigned at = 0; at < SCRIPT_RUNTIME_CPU_PHASES; at++)
        reconciled += cpu.metrics.exclusive_cpu_us[at];
    assert(cpu.depth == 0 && cpu.metrics.errors == 0 && reconciled == 110);
    assert(cpu.metrics.inclusive_cpu_us[SCRIPT_RUNTIME_CPU_JOBS] == 35);
    assert(cpu.metrics.exclusive_cpu_us[SCRIPT_RUNTIME_CPU_JOBS] == 15);
    assert(cpu.metrics.exclusive_cpu_us[SCRIPT_RUNTIME_CPU_ADVANCE] == 25);
    /* A failed clock clears diagnostic nesting, never spins at unwind. */
    validation_cpu_enter(&cpu, SCRIPT_RUNTIME_CPU_ADVANCE, clock_at(120, 120));
    validation_cpu_leave(&cpu, SCRIPT_RUNTIME_CPU_ADVANCE, (ValidationPhaseClock){0});
    assert(cpu.depth == 0 && cpu.metrics.errors != 0);
    cpu = (ValidationCpuCapture){.enabled=true};
    validation_cpu_enter(&cpu, SCRIPT_RUNTIME_CPU_ADVANCE, clock_at(10, 10));
    validation_cpu_leave(&cpu, SCRIPT_RUNTIME_CPU_ADVANCE, clock_at(9, 20));
    assert(cpu.depth == 0 && cpu.metrics.errors != 0);
    cpu = (ValidationCpuCapture){.enabled=true};
    for (unsigned at = 0; at < VALIDATION_CPU_DEPTH; at++)
        validation_cpu_enter(&cpu, SCRIPT_RUNTIME_CPU_JOBS, clock_at(at * 3, at * 3));
    validation_cpu_enter(&cpu, SCRIPT_RUNTIME_CPU_JOBS, clock_at(50, 50));
    assert(cpu.depth == 0 && cpu.metrics.errors != 0);
    capture.mode = 3u;
    uint32_t counts[] = {9u, 17u};
    assert(validation_phase_begin(&capture, 42u, clock_at(100, 1000)));
    assert(validation_phase_enter(&capture, 0u, clock_at(110, 1010)));
    assert(validation_phase_enter(&capture, 1u, clock_at(120, 1020)));
    assert(validation_phase_leave(&capture, 1u, clock_at(150, 1070)));
    assert(validation_phase_leave(&capture, 0u, clock_at(170, 1100)));
    assert(validation_phase_end(&capture, clock_at(180, 1120), 123, 456, counts, 2));
    const ValidationPhaseFrame *frame = &capture.frames[0];
    assert(frame->cpu_us == 80u && frame->wall_us == 120u);
    assert(frame->phases[0].cpu_us == 60u);
    assert(frame->phases[0].exclusive_cpu_us == 30u);
    assert(frame->phases[1].exclusive_cpu_us == 30u);
    assert(frame->phases[1].wall_us == 50u);
    assert(frame->observer_us == 12u && frame->uncertainty_us == 2u);
    assert(frame->sequence == 42u && frame->checksum_b == 456u);
    assert(frame->counters[0] == 9u && frame->counters[1] == 17u);

    /* Recursive IDs count both calls, but exclusive CPU remains disjoint. */
    assert(validation_phase_begin(&capture, 43u, clock_at(200, 1200)));
    assert(validation_phase_enter(&capture, 2u, clock_at(210, 1210)));
    assert(validation_phase_enter(&capture, 2u, clock_at(220, 1220)));
    assert(validation_phase_leave(&capture, 2u, clock_at(230, 1230)));
    assert(validation_phase_leave(&capture, 2u, clock_at(240, 1240)));
    assert(validation_phase_end(&capture, clock_at(250, 1250), 0, 0, NULL, 0));
    assert(capture.frames[1].phases[2].calls == 2u);
    assert(capture.frames[1].phases[2].cpu_us == 40u);
    assert(capture.frames[1].phases[2].exclusive_cpu_us == 30u);

    /* Separate subsystem IDs must not alias HUD/audio scopes. */
    assert(validation_phase_begin(&capture, 44u, clock_at(300, 1300)));
    assert(validation_phase_enter(&capture, 32u, clock_at(310, 1310)));
    assert(validation_phase_enter(&capture, 37u, clock_at(320, 1320)));
    assert(validation_phase_leave(&capture, 37u, clock_at(330, 1330)));
    assert(validation_phase_leave(&capture, 32u, clock_at(340, 1340)));
    assert(validation_phase_end(&capture, clock_at(350, 1350), 0, 0, NULL, 0));
    assert(capture.frames[2].phases[32].exclusive_cpu_us == 20u);
    assert(capture.frames[2].phases[37].exclusive_cpu_us == 10u);

    memset(&capture, 0, sizeof(capture)); capture.mode = 2u;
    assert(validation_phase_begin(&capture, 45u, (ValidationPhaseClock){0}));
    assert(!validation_phase_enter(&capture, VALIDATION_PHASE_LIMIT,
                                   (ValidationPhaseClock){0}));
    assert(capture.errors != 0u);

    memset(&capture, 0, sizeof(capture)); capture.mode = 2u;
    uint32_t extended_counts[40] = {0};
    extended_counts[32] = 23u;
    extended_counts[39] = 41u;
    assert(validation_phase_begin(&capture, 46u, (ValidationPhaseClock){0}));
    assert(validation_phase_end(&capture, (ValidationPhaseClock){0},
                                0, 0, extended_counts, 40u));
    assert(capture.frames[0].counters[32] == 23u);
    assert(capture.frames[0].counters[39] == 41u);

    /* Counters mode must not require or manufacture timing measurements. */
    memset(&capture, 0, sizeof(capture));
    capture.mode = 2u;
    ValidationPhaseClock unavailable = {0};
    assert(validation_phase_begin(&capture, 1, unavailable));
    assert(validation_phase_enter(&capture, 1, unavailable));
    assert(validation_phase_leave(&capture, 1, unavailable));
    assert(validation_phase_end(&capture, unavailable, 0, 0, counts, 2));
    assert(capture.frames[0].cpu_us == 0 && capture.frames[0].observer_us == 0);
    assert(capture.frames[0].phases[1].calls == 1);

    /* Overflow and malformed nesting invalidate the capture rather than
       silently dropping frames, clock failures or negative exclusive CPU. */
    capture.count = VALIDATION_PHASE_FRAME_LIMIT;
    assert(!validation_phase_begin(&capture, 2, unavailable));
    assert(capture.errors != 0);
    memset(&capture, 0, sizeof(capture)); capture.mode = 3;
    assert(validation_phase_begin(&capture, 1, clock_at(1, 1)));
    assert(validation_phase_enter(&capture, 1, clock_at(3, 3)));
    assert(!validation_phase_leave(&capture, 2, clock_at(8, 8)));
    assert(!validation_phase_end(&capture, clock_at(9, 9), 0, 0, NULL, 0));
    memset(&capture, 0, sizeof(capture)); capture.mode = 1;
    assert(!validation_phase_begin(&capture, 1, unavailable));
    memset(&capture, 0, sizeof(capture)); capture.mode = 3;
    assert(validation_phase_begin(&capture, 1, clock_at(0, 0)));
    for (uint32_t at = 0; at < VALIDATION_PHASE_DEPTH; at++)
        assert(validation_phase_enter(&capture, at, clock_at(at * 3 + 3, at * 3 + 3)));
    assert(!validation_phase_enter(&capture, 0, clock_at(50, 50)));
    memset(&capture, 0, sizeof(capture)); capture.mode = 3;
    assert(validation_phase_begin(&capture, 1, clock_at(1, 1)));
    assert(validation_phase_enter(&capture, 0, clock_at(5, 5)));
    assert(!validation_phase_leave(&capture, 0, clock_at(4, 9)));
    memset(&capture, 0, sizeof(capture)); capture.mode = 2;
    assert(validation_phase_begin(&capture, 1, unavailable));
    assert(!validation_phase_end(&capture, unavailable, 0, 0, counts,
                                VALIDATION_PHASE_COUNTER_LIMIT + 1u));
    puts("validation phase nesting, counters and refusal checks passed");
    return 0;
}
