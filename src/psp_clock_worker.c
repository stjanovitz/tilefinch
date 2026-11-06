#include "tilefinch/psp_clock_worker.h"
#include "tilefinch/psp_threads.h"

#include <pspkernel.h>
#include <psppower.h>

#include <limits.h>
#include <string.h>

#include "tilefinch/psp_power_policy.h"
#include "psp_thread_contract.h"

#define CLOCK_WORKER_WAKE 1u
#define CLOCK_WORKER_STACK_BYTES (8u * 1024u)

static int clock_worker_main(SceSize argument_size, void *arguments)
{
    PspClockWorker *worker = NULL;
    if (arguments != NULL && argument_size == sizeof(worker))
        memcpy(&worker, arguments, sizeof(worker));
    if (worker == NULL) return -1;
    while (atomic_load_explicit(
               &worker->stop_requested, memory_order_acquire) == 0) {
        uint32_t bits = 0;
        int wait_result = sceKernelWaitEventFlag(
            worker->event, CLOCK_WORKER_WAKE,
            PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, NULL);
        if (wait_result < 0) {
            atomic_fetch_add_explicit(
                &worker->failures, 1u, memory_order_relaxed);
            break;
        }
        if (atomic_load_explicit(
                &worker->stop_requested, memory_order_acquire) != 0)
            break;
        atomic_store_explicit(
            &worker->transitioning, 1, memory_order_release);
        bool idle = atomic_load_explicit(
            &worker->desired_idle, memory_order_acquire) != 0;
        unsigned cpu = idle
            ? PSP_POWER_POLICY_IDLE_CPU_MHZ
            : PSP_POWER_POLICY_HIGH_CPU_MHZ;
        unsigned bus = idle
            ? PSP_POWER_POLICY_IDLE_BUS_MHZ
            : PSP_POWER_POLICY_HIGH_BUS_MHZ;
        uint64_t started_us = sceKernelGetSystemTimeWide();
        bool already_applied =
            scePowerGetCpuClockFrequencyInt() == (int) cpu
            && scePowerGetBusClockFrequencyInt() == (int) bus;
        int result = already_applied
            ? 0 : scePowerSetClockFrequency(
                      (int) cpu, (int) cpu, (int) bus);
        uint64_t elapsed_us = sceKernelGetSystemTimeWide() - started_us;
        uint32_t bounded_us = elapsed_us > UINT_MAX
            ? UINT_MAX : (uint32_t) elapsed_us;
        atomic_store_explicit(
            &worker->last_transition_us, bounded_us,
            memory_order_release);
        uint32_t maximum = atomic_load_explicit(
            &worker->maximum_transition_us, memory_order_relaxed);
        while (bounded_us > maximum
               && !atomic_compare_exchange_weak_explicit(
                   &worker->maximum_transition_us, &maximum, bounded_us,
                   memory_order_relaxed, memory_order_relaxed)) {
        }
        bool applied = result >= 0
            && scePowerGetCpuClockFrequencyInt() == (int) cpu
            && scePowerGetBusClockFrequencyInt() == (int) bus;
        if (applied) {
            atomic_store_explicit(
                &worker->applied_idle, idle ? 1 : 0,
                memory_order_release);
            atomic_fetch_add_explicit(
                &worker->completions, 1u, memory_order_release);
        } else {
            atomic_fetch_add_explicit(
                &worker->failures, 1u, memory_order_release);
        }
        atomic_store_explicit(
            &worker->transitioning, 0, memory_order_release);
        if (atomic_load_explicit(
                &worker->desired_idle, memory_order_acquire)
                != (idle ? 1 : 0)) {
            (void) sceKernelSetEventFlag(
                worker->event, CLOCK_WORKER_WAKE);
        }
    }
    return 0;
}

#include "psp_clock_worker_lifecycle.inc"

void psp_clock_worker_snapshot(
    const PspClockWorker *worker, PspClockWorkerSnapshot *snapshot)
{
    if (snapshot == NULL) return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (worker == NULL) return;
    snapshot->desired_idle = atomic_load_explicit(
        &worker->desired_idle, memory_order_acquire) != 0;
    snapshot->applied_idle = atomic_load_explicit(
        &worker->applied_idle, memory_order_acquire) != 0;
    snapshot->transitioning = atomic_load_explicit(
        &worker->transitioning, memory_order_acquire) != 0;
    snapshot->completions = atomic_load_explicit(
        &worker->completions, memory_order_acquire);
    snapshot->failures = atomic_load_explicit(
        &worker->failures, memory_order_acquire);
    snapshot->last_transition_us = atomic_load_explicit(
        &worker->last_transition_us, memory_order_acquire);
    snapshot->maximum_transition_us = atomic_load_explicit(
        &worker->maximum_transition_us, memory_order_acquire);
}
