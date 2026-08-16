#ifndef TILEFINCH_PSP_THREAD_CONTRACT_H
#define TILEFINCH_PSP_THREAD_CONTRACT_H

#include <pspkernel.h>
#include <pspkerror.h>

#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t status;
    int exit_status;
} PspThreadObservation;

/* ReferThreadStatus is the firmware-supported nonblocking observation. Keep
   the required size initialization here so a new caller cannot reproduce the
   PPSSPP-green/PSP-illegal-argument class of failure. */
static inline int psp_thread_observe(
    SceUID thread, PspThreadObservation *observation)
{
    if (observation == NULL)
        return (int) SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    int result = sceKernelReferThreadStatus(thread, &info);
    if (result >= 0) {
        observation->status = (uint32_t) info.status;
        observation->exit_status = info.exitStatus;
    } else {
        observation->status = 0;
        observation->exit_status = result;
    }
    return result;
}

/* Timed waits are valid with a positive timeout, but a pointer to zero is not
   a portable poll on 6.6x firmware. Force polling through psp_thread_observe
   and reject the bad argument before it reaches the syscall. */
static inline int psp_thread_wait_end_bounded(
    SceUID thread, SceUInt timeout_us)
{
    if (timeout_us == 0)
        return (int) SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
    return sceKernelWaitThreadEnd(thread, &timeout_us);
}

#endif
