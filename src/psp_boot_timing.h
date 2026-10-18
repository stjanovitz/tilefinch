#ifndef TILEFINCH_PSP_BOOT_TIMING_H
#define TILEFINCH_PSP_BOOT_TIMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Validation-only argv receipt: no files, pointers or private page data.
 * LoadExec resets kernel uptime. Use persistent RTC ticks at exactly 1 MHz
 * for cross-process intervals; kernel time remains suitable within a process. */
#define PSP_BOOT_TIMING_PREFIX "--tilefinch-boot-us="
#define PSP_BOOT_TIMING_ARGUMENT_BYTES (sizeof(PSP_BOOT_TIMING_PREFIX) + 33u)

typedef struct {
    uint64_t launcher_us;
    uint64_t handoff_us;
} PspBootHandoffTiming;

#ifdef __PSP__
#include <psprtc.h>
static inline bool psp_boot_timing_clock(uint64_t *output)
{
    if (output == NULL) return false;
    *output = 0;
    if (sceRtcGetTickResolution() == 1000000u
        && sceRtcGetCurrentTick(output) >= 0 && *output != 0) return true;
    /* A refused syscall may still have written its output parameter. */
    *output = 0;
    return false;
}
#endif

static inline void psp_boot_timing_encode(
    char output[PSP_BOOT_TIMING_ARGUMENT_BYTES], PspBootHandoffTiming timing)
{
    static const char digits[] = "0123456789abcdef";
    size_t at = sizeof(PSP_BOOT_TIMING_PREFIX) - 1u;
    memcpy(output, PSP_BOOT_TIMING_PREFIX, at);
    const uint64_t values[] = {timing.launcher_us, timing.handoff_us};
    for (size_t value = 0; value < 2u; value++) {
        for (unsigned nibble = 0; nibble < 16u; nibble++)
            output[at++] = digits[(values[value] >> (60u - nibble * 4u)) & 15u];
        if (value == 0u) output[at++] = '/';
    }
    output[at] = '\0';
}

static inline bool psp_boot_timing_decode(
    const char *argument, uint64_t entered_us, PspBootHandoffTiming *output)
{
    if (argument == NULL || output == NULL) return false;
    size_t at = 0;
    for (; at < sizeof(PSP_BOOT_TIMING_PREFIX) - 1u; at++)
        if (argument[at] != PSP_BOOT_TIMING_PREFIX[at]) return false;
    uint64_t values[2] = {0, 0};
    for (size_t value = 0; value < 2u; value++) {
        for (unsigned nibble = 0; nibble < 16u; nibble++) {
            unsigned char digit = (unsigned char) argument[at++];
            unsigned decoded = digit >= '0' && digit <= '9'
                ? (unsigned) (digit - '0')
                : digit >= 'a' && digit <= 'f'
                    ? (unsigned) (digit - 'a') + 10u : 16u;
            if (decoded == 16u) return false;
            values[value] = (values[value] << 4u) | decoded;
        }
        if (value == 0u && argument[at++] != '/') return false;
    }
    if (argument[at] != '\0' || values[0] == 0 || values[1] == 0
        || entered_us == 0 || values[0] > values[1]
        || values[1] > entered_us
        || entered_us - values[0] > UINT64_C(600000000)) return false;
    *output = (PspBootHandoffTiming) {values[0], values[1]};
    return true;
}

#endif
