#ifndef TILEFINCH_PSP_NETWORK_H
#define TILEFINCH_PSP_NETWORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PSP_NETWORK_IDLE = 0,
    PSP_NETWORK_CHECKING_PROFILE,
    PSP_NETWORK_LOADING_COMMON,
    PSP_NETWORK_LOADING_INET,
    PSP_NETWORK_INITIALIZING_CORE,
    PSP_NETWORK_INITIALIZING_INET,
    PSP_NETWORK_INITIALIZING_RESOLVER,
    PSP_NETWORK_INITIALIZING_APCTL,
    PSP_NETWORK_CONNECTING,
    PSP_NETWORK_WAITING_FOR_IP,
    PSP_NETWORK_READY,
    PSP_NETWORK_FAILED,
    PSP_NETWORK_CANCELLED,
    PSP_NETWORK_STATUS_COUNT
} PspNetworkStatus;

typedef struct {
    PspNetworkStatus status;
    PspNetworkStatus failure_phase;
    int profile_index;
    int apctl_state;
    int native_result;
    uint64_t started_us;
    uint64_t elapsed_us;
    uint64_t last_pump_us;
    uint64_t maximum_pump_us;
    uint64_t phase_pump_us[PSP_NETWORK_STATUS_COUNT];
    size_t phase_pump_calls[PSP_NETWORK_STATUS_COUNT];
    size_t pump_calls;
    size_t free_memory_start;
    size_t free_memory_minimum;
    size_t free_memory_ready;
    size_t maximum_free_block_start;
    size_t maximum_free_block_minimum;
    size_t maximum_free_block_ready;
    uint32_t profile_query_success_mask;
    uint32_t profile_query_failure_mask;
    uint32_t initialization_adopted_mask;
    unsigned profile_security_type;
    bool profile_static_ip;
    bool profile_manual_dns;
    bool profile_uses_proxy;
    int wlan_switch_state;
    int wlan_power_state;
    PspNetworkStatus maximum_pump_phase;
    bool common_loaded;
    bool inet_loaded;
    bool common_owned;
    bool inet_owned;
    bool core_initialized;
    bool inet_initialized;
    bool resolver_initialized;
    bool apctl_initialized;
    bool connect_started;
    bool disconnect_started;
} PspNetwork;

typedef struct {
    uint32_t query_success_mask;
    uint32_t query_failure_mask;
    int first_error;
    unsigned security_type;
    unsigned signal_strength;
    unsigned channel;
    bool power_save;
    bool uses_proxy;
    bool has_ip;
    bool has_subnet;
    bool has_gateway;
    bool has_primary_dns;
    bool has_secondary_dns;
} PspNetworkInterfaceReport;

typedef struct {
    uint32_t attempted_mask;
    uint32_t failure_mask;
    int first_error;
    int final_apctl_state;
    size_t disconnect_polls;
    uint64_t disconnect_wait_us;
    uint64_t elapsed_us;
    size_t free_memory_before;
    size_t free_memory_after;
    size_t maximum_free_block_before;
    size_t maximum_free_block_after;
} PspNetworkShutdownReport;

typedef enum {
    PSP_NETWORK_SHUTDOWN_IDLE = 0,
    PSP_NETWORK_SHUTDOWN_LEAVING,
    PSP_NETWORK_SHUTDOWN_TERM_APCTL,
    PSP_NETWORK_SHUTDOWN_TERM_RESOLVER,
    PSP_NETWORK_SHUTDOWN_TERM_INET,
    PSP_NETWORK_SHUTDOWN_TERM_CORE,
    PSP_NETWORK_SHUTDOWN_UNLOAD_INET,
    PSP_NETWORK_SHUTDOWN_UNLOAD_COMMON,
    PSP_NETWORK_SHUTDOWN_COMPLETE
} PspNetworkShutdownPhase;

typedef struct {
    PspNetworkShutdownPhase phase;
    PspNetworkShutdownReport report;
    uint64_t started_us;
    uint64_t leave_started_us;
    bool leave_timed_out;
} PspNetworkShutdownOperation;

typedef enum {
    PSP_NETWORK_REJOIN_IDLE = 0,
    PSP_NETWORK_REJOIN_LEAVING,
    PSP_NETWORK_REJOIN_WAITING_IP,
    PSP_NETWORK_REJOIN_COMPLETE,
    PSP_NETWORK_REJOIN_FAILED
} PspNetworkRejoinServicePhase;

typedef struct {
    PspNetworkRejoinServicePhase phase;
    uint64_t started_us;
    uint64_t phase_started_us;
    int native_result;
} PspNetworkRejoinOperation;

bool psp_network_begin(PspNetwork *network, int profile_index);
PspNetworkStatus psp_network_pump(PspNetwork *network,
                                  uint64_t timeout_us);
/* Stop advancing Starting. The outer supervisor owns APCTL leave/unwind. */
void psp_network_mark_cancelled(PspNetwork *network);
bool psp_network_interface_report(
    const PspNetwork *network, PspNetworkInterfaceReport *report);
/*
 * Revalidate an established APCTL session after system resume. Firmware may
 * retain the modules and IP lease; callers only need a full shutdown/reconnect
 * when this bounded state/interface check fails.
 */
bool psp_network_resume_ready(
    PspNetwork *network, int *native_result);
/* Cheap steady-state probe: APCTL + physical WLAN facts only. */
bool psp_network_link_ready(PspNetwork *network, int *native_result);
/* Bounded APCTL-only disconnect/reconnect service. The initialized rungs stay
   resident; failure is escalated by the outer supervisor to a full restart. */
void psp_network_rejoin_begin(
    PspNetwork *network, PspNetworkRejoinOperation *operation);
bool psp_network_rejoin_pump(
    PspNetwork *network, PspNetworkRejoinOperation *operation,
    uint64_t timeout_us);
/* One firmware operation or APCTL poll per pump. The stack is cleared only
   after every owned rung has been considered. */
void psp_network_shutdown_begin(
    PspNetwork *network, PspNetworkShutdownOperation *operation);
bool psp_network_shutdown_pump(
    PspNetwork *network, PspNetworkShutdownOperation *operation);
void psp_network_shutdown(
    PspNetwork *network, PspNetworkShutdownReport *report);
const char *psp_network_status_name(PspNetworkStatus status);

#endif
