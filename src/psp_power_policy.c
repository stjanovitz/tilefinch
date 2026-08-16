#include "tilefinch/psp_power_policy.h"

#include <string.h>

static bool power_policy_set(PspPowerPolicy *policy, bool idle)
{
    if (policy == NULL || policy->backend.set_clock == NULL) return false;
    uint64_t elapsed_us = 0;
    unsigned cpu = idle
        ? PSP_POWER_POLICY_IDLE_CPU_MHZ : PSP_POWER_POLICY_HIGH_CPU_MHZ;
    unsigned bus = idle
        ? PSP_POWER_POLICY_IDLE_BUS_MHZ : PSP_POWER_POLICY_HIGH_BUS_MHZ;
    if (!policy->backend.set_clock(
            policy->backend.context, cpu, bus, &elapsed_us)) {
        policy->failures++;
        return false;
    }
    policy->idle_clock = idle;
    policy->transitions++;
    policy->last_transition_us = elapsed_us;
    if (elapsed_us > policy->maximum_transition_us)
        policy->maximum_transition_us = elapsed_us;
    return true;
}

void psp_power_policy_init(
    PspPowerPolicy *policy, PspPowerPolicyBackend backend)
{
    if (policy == NULL) return;
    memset(policy, 0, sizeof(*policy));
    policy->backend = backend;
}

bool psp_power_policy_update(
    PspPowerPolicy *policy, unsigned elapsed_ms,
    bool interaction, bool active_work)
{
    if (policy == NULL) return false;
    if (interaction || active_work) {
        policy->quiet_ms = 0;
        return policy->idle_clock
            ? power_policy_set(policy, false) : false;
    }
    if (policy->idle_clock) return false;
    unsigned remaining =
        PSP_POWER_POLICY_IDLE_DELAY_MS - policy->quiet_ms;
    if (elapsed_ms >= remaining) {
        policy->quiet_ms = PSP_POWER_POLICY_IDLE_DELAY_MS;
        return power_policy_set(policy, true);
    }
    policy->quiet_ms += elapsed_ms;
    return false;
}
