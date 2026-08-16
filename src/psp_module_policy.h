#ifndef TILEFINCH_PSP_MODULE_POLICY_H
#define TILEFINCH_PSP_MODULE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * PSP utility and kernel module managers use negative status words for some
 * successful idempotent outcomes. Keep that classification in one pure,
 * host-tested seam: treating every negative result as failure broke repeated
 * AV and network setup, while accepting any negative result risks continuing
 * without the required exports.
 */
#define PSP_MODULE_NET_ALREADY_LOADED UINT32_C(0x80111102)
#define PSP_MODULE_AV_ALREADY_LOADED UINT32_C(0x80110f02)
#define PSP_MODULE_KERNEL_ALREADY_STARTED UINT32_C(0x80020133)
#define PSP_MODULE_KERNEL_EXCLUSIVE_LOAD UINT32_C(0x80020139)
#define PSP_MODULE_ERROR_THREAD_TERMINATED UINT32_C(0x800201ac)

/* SceKernelThreadInfo::status values, mirrored here so host policy tests do
   not need the PSP-only thread-manager headers. */
#define PSP_MODULE_THREAD_STATUS_STOPPED UINT32_C(0x10)
#define PSP_MODULE_THREAD_STATUS_KILLED UINT32_C(0x20)

typedef enum {
    PSP_UTILITY_MODULE_LOAD_FAILED = 0,
    PSP_UTILITY_MODULE_LOAD_ACQUIRED,
    PSP_UTILITY_MODULE_LOAD_RESIDENT
} PspUtilityModuleLoadDisposition;

typedef enum {
    PSP_MODULE_WORKER_POLL_ERROR = 0,
    PSP_MODULE_WORKER_POLL_PENDING,
    PSP_MODULE_WORKER_POLL_TERMINAL
} PspModuleWorkerPollDisposition;

static inline PspUtilityModuleLoadDisposition
psp_utility_net_module_load_disposition(int status)
{
    if (status >= 0) return PSP_UTILITY_MODULE_LOAD_ACQUIRED;
    if ((uint32_t) status == PSP_MODULE_NET_ALREADY_LOADED)
        return PSP_UTILITY_MODULE_LOAD_RESIDENT;
    return PSP_UTILITY_MODULE_LOAD_FAILED;
}

static inline PspUtilityModuleLoadDisposition
psp_utility_av_module_load_disposition(int status)
{
    if (status >= 0) return PSP_UTILITY_MODULE_LOAD_ACQUIRED;
    /* sceUtilityLoadAvModule appears to have its own error family: PPSSPP
       returns 0x80110f02 for an already-resident AV module rather than the
       0x80111102 used by network/general utility modules. Which code real
       6.61 firmware returns is UNCONFIRMED on hardware, and the second AV
       load is reachable (a suspend can kill the module worker mid-load with
       the module actually resident). An unrecognized code classifies FATAL
       and latches media off for the whole process, so accept both codes
       defensively here. The network classifier keeps its exact single code:
       nothing observed returns the AV value for a network module. */
    if ((uint32_t) status == PSP_MODULE_AV_ALREADY_LOADED
        || (uint32_t) status == PSP_MODULE_NET_ALREADY_LOADED)
        return PSP_UTILITY_MODULE_LOAD_RESIDENT;
    return PSP_UTILITY_MODULE_LOAD_FAILED;
}

static inline bool psp_utility_module_load_owned(
    PspUtilityModuleLoadDisposition disposition)
{
    return disposition == PSP_UTILITY_MODULE_LOAD_ACQUIRED;
}

static inline bool psp_kernel_module_already_resident(int status)
{
    return (uint32_t) status == PSP_MODULE_KERNEL_EXCLUSIVE_LOAD;
}

static inline bool psp_kernel_module_start_succeeded(
    int call_status, int module_status)
{
    if ((uint32_t) call_status == PSP_MODULE_KERNEL_ALREADY_STARTED)
        return true;
    return call_status >= 0 && module_status >= 0;
}

static inline PspModuleWorkerPollDisposition
psp_module_worker_poll_disposition(int refer_status, uint32_t thread_status)
{
    if (refer_status < 0) return PSP_MODULE_WORKER_POLL_ERROR;
    if ((thread_status & (PSP_MODULE_THREAD_STATUS_STOPPED
                          | PSP_MODULE_THREAD_STATUS_KILLED)) != 0)
        return PSP_MODULE_WORKER_POLL_TERMINAL;
    return PSP_MODULE_WORKER_POLL_PENDING;
}

static inline bool psp_module_worker_was_killed(uint32_t thread_status)
{
    return (thread_status & PSP_MODULE_THREAD_STATUS_KILLED) != 0;
}

/* A worker which was required to signal completion but became terminal
   without doing so has failed even when its recorded exit status is zero.
   Preserve a real negative exit status; otherwise use the stable thread-
   terminated error so callers never turn a silent worker death into an
   infinite WOULD_BLOCK/poll loop. */
static inline int psp_unexpected_worker_exit_status(
    uint32_t thread_status, int exit_status)
{
    if (exit_status < 0) return exit_status;
    (void) thread_status;
    return (int) PSP_MODULE_ERROR_THREAD_TERMINATED;
}

/* SystemCtrl's function lookup is a weak CFW import. On firmware where the
   library is absent, the unresolved stub can return a negative PSP error word
   rather than the documented zero. Never reinterpret that status as a code
   pointer and pass it to kuKernelCall. PSP kernel code uses the cached
   0x88xxxxxx-0x8fxxxxxx mapping and MIPS instructions are word aligned. */
static inline bool psp_kernel_callable_address(uint32_t address)
{
    return (address & UINT32_C(0xf8000003)) == UINT32_C(0x88000000);
}

#endif
