#include "tilefinch/psp_network.h"

#include <stdarg.h>
#include <stdio.h>

static bool enabled;
void psp_network_diagnostics_enable(bool value) { enabled = value; }
bool psp_network_diagnostics_enabled(void) { return enabled; }

void psp_network_diagnostics_record(PspNetwork *network)
{
    if (network == NULL || !network->diagnostics_enabled) return;
    unsigned count = network->diagnostic_count;
    if (count != 0 && count <= PSP_NETWORK_DIAGNOSTIC_EVENTS) {
        const PspNetworkDiagnosticEvent *last = &network->diagnostic_events[count - 1u];
        if (last->phase == network->status && last->apctl_state == network->apctl_state
            && last->native_result == network->native_result) return;
    }
    if (count >= PSP_NETWORK_DIAGNOSTIC_EVENTS) {
        if (network->diagnostic_dropped != UINT32_MAX) network->diagnostic_dropped++;
        return;
    }
    network->diagnostic_events[count] = (PspNetworkDiagnosticEvent) {
        network->elapsed_us, network->status, network->apctl_state,
        network->native_result
    };
    network->diagnostic_count++;
}

static bool append(char *output, size_t capacity, size_t *used,
                   const char *format, ...)
{
    if (*used >= capacity) return false;
    va_list args;
    va_start(args, format);
    int count = vsnprintf(output + *used, capacity - *used, format, args);
    va_end(args);
    if (count < 0 || (size_t) count >= capacity - *used) return false;
    *used += (size_t) count;
    return true;
}

bool psp_network_diagnostics_format(const PspNetwork *network,
                                   char *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return false;
    output[0] = '\0';
    if (network == NULL || !network->diagnostics_enabled
        || network->diagnostic_count > PSP_NETWORK_DIAGNOSTIC_EVENTS) return false;
    size_t used = 0;
    bool okay = append(output, capacity, &used,
        "tilefinch-wifi-v1\nstatus=%d failed-phase=%d native=0x%08x\n"
        "profile=%d/%d fallback=%d security=%u static-ip=%d manual-dns=%d proxy=%d\n"
        "wlan-switch=%d power=%d apctl=%d\n"
        "profile-query-ok=0x%08x failed=0x%08x adopted=0x%08x\n"
        "elapsed-us=%llu attempts=%u resets=%u pumps=%zu max-pump-us=%llu\n"
        "free-start=%zu minimum=%zu ready=%zu block-start=%zu minimum=%zu ready=%zu\n"
        "events=%u dropped=%u\n",
        (int) network->status, (int) network->failure_phase, (unsigned) network->native_result,
        network->requested_profile_index, network->profile_index,
        network->profile_fallback_used, network->profile_security_type,
        network->profile_static_ip, network->profile_manual_dns, network->profile_uses_proxy,
        network->wlan_switch_state, network->wlan_power_state, network->apctl_state,
        (unsigned) network->profile_query_success_mask,
        (unsigned) network->profile_query_failure_mask,
        (unsigned) network->initialization_adopted_mask,
        (unsigned long long) network->elapsed_us, network->join_attempts,
        network->join_resets, network->pump_calls,
        (unsigned long long) network->maximum_pump_us,
        network->free_memory_start, network->free_memory_minimum, network->free_memory_ready,
        network->maximum_free_block_start, network->maximum_free_block_minimum,
        network->maximum_free_block_ready, network->diagnostic_count, network->diagnostic_dropped);
    for (unsigned at = 0; okay && at < PSP_NETWORK_STATUS_COUNT; at++) {
        if (network->phase_pump_calls[at] == 0) continue;
        okay = append(output, capacity, &used, "phase=%u calls=%zu pump-us=%llu\n",
            at, network->phase_pump_calls[at],
            (unsigned long long) network->phase_pump_us[at]);
    }
    for (unsigned at = 0; okay && at < network->diagnostic_count; at++) {
        const PspNetworkDiagnosticEvent *event = &network->diagnostic_events[at];
        okay = append(output, capacity, &used, "event=%llu phase=%d apctl=%d native=0x%08x\n",
            (unsigned long long) event->elapsed_us, (int) event->phase,
            event->apctl_state, (unsigned) event->native_result);
    }
    if (!okay) output[0] = '\0'; /* Never export a silently truncated report. */
    return okay;
}
