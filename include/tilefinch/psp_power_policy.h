#ifndef TILEFINCH_PSP_POWER_POLICY_H
#define TILEFINCH_PSP_POWER_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define PSP_POWER_POLICY_HIGH_CPU_MHZ 333u
#define PSP_POWER_POLICY_HIGH_BUS_MHZ 166u
#define PSP_POWER_POLICY_IDLE_CPU_MHZ 222u
#define PSP_POWER_POLICY_IDLE_BUS_MHZ 111u
#define PSP_POWER_POLICY_IDLE_DELAY_MS 2000u

typedef bool (*PspPowerPolicySetClock)(
    void *context, unsigned cpu_mhz, unsigned bus_mhz,
    uint64_t *transition_us);

typedef struct {
    PspPowerPolicySetClock set_clock;
    void *context;
} PspPowerPolicyBackend;

typedef struct {
    PspPowerPolicyBackend backend;
    unsigned quiet_ms;
    bool idle_clock;
    unsigned transitions;
    unsigned failures;
    uint64_t last_transition_us;
    uint64_t maximum_transition_us;
} PspPowerPolicy;

void psp_power_policy_init(
    PspPowerPolicy *policy, PspPowerPolicyBackend backend);
/*
 * `active_work` covers loading, scrolling, media, voice, and other bounded
 * jobs. `interaction` is separate so a low-clock policy ramps synchronously
 * before the caller dispatches the input which caused it.
 */
bool psp_power_policy_update(
    PspPowerPolicy *policy, unsigned elapsed_ms,
    bool interaction, bool active_work);

#endif
