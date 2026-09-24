#ifndef TILEFINCH_PSP_FPU_H
#define TILEFINCH_PSP_FPU_H

#include <stdint.h>

/*
 * PSP threads start with FPU exception traps enabled, so a single-precision
 * ordered comparison or conversion involving NaN raises a CPU exception and
 * stops the thread (found on a PSP-3000: WebGL vertex validation compared a
 * NaN position and the browser hung). The engine is written and tested
 * against IEEE default behaviour with exceptions masked, as on the host, and
 * web content can supply NaN almost anywhere. Call this first in every
 * thread that runs float code.
 *
 * Only the exception-enable (bits 7-11) and cause (bits 12-17) fields are
 * cleared. Rounding mode and flush-to-zero are left as the firmware set
 * them, unlike pspSdkDisableFPUExceptions, which clears those too.
 */
#if defined(__PSP__)
static inline void psp_fpu_mask_exceptions(void)
{
    unsigned int status;
    __asm__ volatile("cfc1 %0, $31" : "=r"(status));
    status &= ~UINT32_C(0x0003ff80);
    __asm__ volatile("ctc1 %0, $31" : : "r"(status));
}
#else
static inline void psp_fpu_mask_exceptions(void) {}
#endif

#endif
