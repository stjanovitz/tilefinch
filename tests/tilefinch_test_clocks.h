#ifndef TILEFINCH_TEST_CLOCKS_H
#define TILEFINCH_TEST_CLOCKS_H

/* Injected platform clocks for test binaries (tilefinch_test_common.h and
   browser_engine_test_support.h include this). Install one with
   tilefinch_platform_set_services() and remove it with
   tilefinch_platform_set_services(NULL). */

#include <stdint.h>
#include <time.h>

#include "tilefinch/platform.h"

/* Injected platform clocks for checks whose subject is work counts or event
   ordering, but whose code under test also samples the monotonic clock
   (8 ms cooperative slices, timer wheels, script watchdogs). On a loaded
   host a real clock lets preemption add yields or fire timers between two
   statements, so such checks install one of these instead.

   The frozen clock never moves: time-sliced yields never trigger and only
   work quotas decide when code yields.

   The counting clock advances a fixed step on every read, so "time" is a
   count of clock reads. A JavaScript loop spinning on performance.now()
   still terminates, and every gap the test measures depends only on the
   code path, not on the scheduler. Wall time reads share the counter. */
typedef struct {
    uint64_t now_ns;
    uint64_t step_ns;
} TestCountingClock;

static inline uint64_t test_frozen_clock_ns(void *context)
{
    (void) context;
    return UINT64_C(1000000000);
}

static inline uint64_t test_counting_clock_ns(void *context)
{
    TestCountingClock *clock = context;
    clock->now_ns += clock->step_ns;
    return clock->now_ns;
}

static inline uint64_t test_counting_clock_wall_ns(void *context)
{
    /* 2023-11-14T22:13:20Z plus the shared count: a plausible epoch. */
    return UINT64_C(1700000000000000000) + test_counting_clock_ns(context);
}

/* The CPU-paced clock is for ratchets whose subject is the cost of one
   bounded unit of work, such as "no transform slice takes 50 ms". It starts
   at the real monotonic time when armed and then advances only while this
   process runs on a CPU (CLOCK_PROCESS_CPUTIME_ID). A loaded host that
   preempts the process mid-slice inflates a real clock by the time other
   processes ran, but not this one. Arming at the real time keeps the clock
   monotonic across installation, so ages recorded before it (cache entries,
   identities) stay meaningful. It still counts only work, so a slice that
   grows without bound still shows up; slower cores and cold caches can
   scale it a few times, which the bounds must allow for. */
typedef struct {
    uint64_t base_ns;
    uint64_t cpu_base_ns;
} TestCpuPacedClock;

static inline uint64_t test_process_cpu_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) != 0) return 0;
    return (uint64_t) now.tv_sec * UINT64_C(1000000000)
        + (uint64_t) now.tv_nsec;
}

/* `real_monotonic_ns` is tilefinch_platform_monotonic_time_ns() read
   before the clock is installed. */
static inline void test_cpu_paced_clock_arm(
    TestCpuPacedClock *clock, uint64_t real_monotonic_ns)
{
    clock->base_ns = real_monotonic_ns;
    clock->cpu_base_ns = test_process_cpu_ns();
}

static inline uint64_t test_cpu_paced_clock_ns(void *context)
{
    const TestCpuPacedClock *clock = context;
    uint64_t cpu_ns = test_process_cpu_ns();
    return clock->base_ns
        + (cpu_ns > clock->cpu_base_ns ? cpu_ns - clock->cpu_base_ns : 0);
}

/* Arm `clock` at the real monotonic time and install it as the platform
   monotonic clock; tilefinch_platform_set_services(NULL) removes it. The
   clock must outlive the installation. */
static inline void test_cpu_paced_clock_install(TestCpuPacedClock *clock)
{
    test_cpu_paced_clock_arm(clock, tilefinch_platform_monotonic_time_ns());
    TilefinchPlatformServices services = {
        .context = clock,
        .monotonic_time_ns = test_cpu_paced_clock_ns
    };
    tilefinch_platform_set_services(&services);
}

#endif
