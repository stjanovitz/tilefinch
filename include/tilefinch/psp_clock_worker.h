#ifndef TILEFINCH_PSP_CLOCK_WORKER_H
#define TILEFINCH_PSP_CLOCK_WORKER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

typedef struct {
    int32_t thread;
    int32_t event;
    atomic_int desired_idle;
    atomic_int applied_idle;
    atomic_int transitioning;
    atomic_int stop_requested;
    atomic_uint completions;
    atomic_uint failures;
    atomic_uint last_transition_us;
    atomic_uint maximum_transition_us;
    bool started;
} PspClockWorker;

typedef struct {
    bool desired_idle;
    bool applied_idle;
    bool transitioning;
    unsigned completions;
    unsigned failures;
    uint32_t last_transition_us;
    uint32_t maximum_transition_us;
} PspClockWorkerSnapshot;

bool psp_clock_worker_start(PspClockWorker *worker);
bool psp_clock_worker_request(
    PspClockWorker *worker, bool idle, uint64_t *request_us);
bool psp_clock_worker_wait_applied(
    PspClockWorker *worker, bool idle, unsigned timeout_ms);
void psp_clock_worker_snapshot(
    const PspClockWorker *worker, PspClockWorkerSnapshot *snapshot);
bool psp_clock_worker_shutdown(PspClockWorker *worker);

#endif
