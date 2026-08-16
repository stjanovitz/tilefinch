#ifndef TILEFINCH_PSP_NETWORK_SUPERVISOR_H
#define TILEFINCH_PSP_NETWORK_SUPERVISOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Pure outer lifecycle model for the PSP networking stack.
 *
 * The existing PspNetworkStatus ladder remains the implementation of the
 * Starting service.  This reducer owns only lifecycle decisions.  It never
 * calls firmware, reads atomics or waits: the frontend samples a snapshot
 * once, applies the returned bounded commands, and reports completions as
 * later events.
 */

typedef enum {
    PSP_NETWORK_SUPERVISOR_OFF = 0,
    PSP_NETWORK_SUPERVISOR_STARTING,
    PSP_NETWORK_SUPERVISOR_READY,
    PSP_NETWORK_SUPERVISOR_REJOINING,
    PSP_NETWORK_SUPERVISOR_STOPPING,
    PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED,
    PSP_NETWORK_SUPERVISOR_SUSPENDED_OFF,
    PSP_NETWORK_SUPERVISOR_OFFLINE,
    PSP_NETWORK_SUPERVISOR_STATE_COUNT
} PspNetworkSupervisorState;

typedef enum {
    PSP_NETWORK_STACK_NONE = 0,
    PSP_NETWORK_STACK_PARTIAL,
    PSP_NETWORK_STACK_FULL,
    /* Full stack retained because a consumer may still be executing in it. */
    PSP_NETWORK_STACK_RETAINED_WEDGED
} PspNetworkStackState;

typedef enum {
    PSP_NETWORK_TARGET_OFF = 0,
    PSP_NETWORK_TARGET_READY,
    PSP_NETWORK_TARGET_SUSPEND
} PspNetworkTargetKind;

typedef enum {
    PSP_NETWORK_TARGET_CAUSE_NONE = 0,
    PSP_NETWORK_TARGET_CAUSE_BOOT,
    PSP_NETWORK_TARGET_CAUSE_NAVIGATION,
    PSP_NETWORK_TARGET_CAUSE_VOICE,
    PSP_NETWORK_TARGET_CAUSE_SUSPEND,
    PSP_NETWORK_TARGET_CAUSE_SHUTDOWN
} PspNetworkTargetCause;

typedef struct {
    PspNetworkTargetKind kind;
    PspNetworkTargetCause cause;
    int profile_index;
} PspNetworkTarget;

typedef enum {
    PSP_NETWORK_REQUEST_BOOT = 0,
    PSP_NETWORK_REQUEST_NAVIGATION,
    PSP_NETWORK_REQUEST_VOICE_INHIBIT,
    PSP_NETWORK_REQUEST_SUSPEND_INHIBIT,
    PSP_NETWORK_REQUEST_SHUTDOWN_INHIBIT,
    PSP_NETWORK_REQUEST_COUNT
} PspNetworkRequester;

typedef struct {
    bool active;
    uint64_t generation;
    int profile_index;
} PspNetworkRequest;

typedef struct {
    PspNetworkRequest requests[PSP_NETWORK_REQUEST_COUNT];
} PspNetworkRequestTable;

typedef enum {
    PSP_NETWORK_REJOIN_NONE = 0,
    PSP_NETWORK_REJOIN_PROBING,
    PSP_NETWORK_REJOIN_APCTL
} PspNetworkRejoinPhase;

typedef enum {
    PSP_NETWORK_STOP_NONE = 0,
    PSP_NETWORK_STOP_ADMISSION,
    PSP_NETWORK_STOP_DRAIN_LEASES,
    PSP_NETWORK_STOP_LEASE_WEDGED,
    PSP_NETWORK_STOP_LEAVE_APCTL,
    PSP_NETWORK_STOP_UNWIND_RUNGS
} PspNetworkStoppingPhase;

typedef enum {
    PSP_NETWORK_STOP_TARGET_NONE = 0,
    PSP_NETWORK_STOP_TARGET_OFF,
    PSP_NETWORK_STOP_TARGET_OFFLINE,
    PSP_NETWORK_STOP_TARGET_SUSPENDED_OFF,
    PSP_NETWORK_STOP_TARGET_SUSPENDED_RETAINED,
    PSP_NETWORK_STOP_TARGET_RESTART
} PspNetworkStoppingTarget;

typedef enum {
    PSP_NETWORK_FAILURE_NONE = 0,
    PSP_NETWORK_FAILURE_START,
    PSP_NETWORK_FAILURE_REJOIN,
    PSP_NETWORK_FAILURE_WLAN_OFF,
    PSP_NETWORK_FAILURE_LEASE_WEDGED,
    PSP_NETWORK_FAILURE_CANCELLED
} PspNetworkFailureKind;

typedef enum {
    PSP_NETWORK_APCTL_UNKNOWN = 0,
    PSP_NETWORK_APCTL_DISCONNECTED,
    PSP_NETWORK_APCTL_ASSOCIATING,
    PSP_NETWORK_APCTL_GOT_IP
} PspNetworkApctlState;

typedef enum {
    PSP_NETWORK_LADDER_DOWN = 0,
    PSP_NETWORK_LADDER_RUNNING,
    PSP_NETWORK_LADDER_READY,
    PSP_NETWORK_LADDER_FAILED
} PspNetworkLadderState;

typedef struct {
    PspNetworkApctlState apctl;
    PspNetworkLadderState ladder;
    PspNetworkStackState stack;
    uint16_t lease_count;
    bool wlan_switch_on;
} PspNetworkSnapshot;

typedef enum {
    PSP_NETWORK_EVENT_NONE = 0,
    PSP_NETWORK_EVENT_TARGET_CHANGED,
    PSP_NETWORK_EVENT_RESUME,
    PSP_NETWORK_EVENT_LADDER_READY,
    PSP_NETWORK_EVENT_LADDER_FAILED,
    PSP_NETWORK_EVENT_REGRESSION_HINT,
    PSP_NETWORK_EVENT_PROBE_HEALTHY,
    PSP_NETWORK_EVENT_PROBE_FAILED,
    PSP_NETWORK_EVENT_REJOINED,
    PSP_NETWORK_EVENT_REJOIN_FAILED,
    PSP_NETWORK_EVENT_ADMISSION_STOPPED,
    PSP_NETWORK_EVENT_LEASES_RELEASED,
    PSP_NETWORK_EVENT_DRAIN_TIMEOUT,
    PSP_NETWORK_EVENT_APCTL_DISCONNECTED,
    PSP_NETWORK_EVENT_LEAVE_TIMEOUT,
    PSP_NETWORK_EVENT_RUNG_UNWOUND,
    PSP_NETWORK_EVENT_UNWOUND,
    PSP_NETWORK_EVENT_RETRY,
    PSP_NETWORK_EVENT_COUNT
} PspNetworkSupervisorEventType;

typedef struct {
    PspNetworkSupervisorEventType type;
    PspNetworkTarget target;
    PspNetworkFailureKind failure;
} PspNetworkSupervisorEvent;

typedef struct {
    PspNetworkSupervisorState state;
    PspNetworkStackState stack;
    PspNetworkTarget target;
    PspNetworkRejoinPhase rejoin_phase;
    PspNetworkStoppingPhase stopping_phase;
    PspNetworkStoppingTarget stopping_target;
    PspNetworkFailureKind failure;
    uint8_t rejoin_attempts;
    uint8_t rejoin_budget;
    uint16_t retained_lease_count;
    bool admission_open;
} PspNetworkSupervisor;

enum {
    PSP_NETWORK_COMMAND_NONE = 0,
    PSP_NETWORK_COMMAND_START_LADDER = 1u << 0,
    PSP_NETWORK_COMMAND_PUMP_LADDER = 1u << 1,
    PSP_NETWORK_COMMAND_OPEN_ADMISSION = 1u << 2,
    PSP_NETWORK_COMMAND_STOP_ADMISSION = 1u << 3,
    PSP_NETWORK_COMMAND_CANCEL_CONSUMERS = 1u << 4,
    PSP_NETWORK_COMMAND_PROBE_LINK = 1u << 5,
    PSP_NETWORK_COMMAND_REJOIN_APCTL = 1u << 6,
    PSP_NETWORK_COMMAND_DISCONNECT_APCTL = 1u << 7,
    PSP_NETWORK_COMMAND_POLL_APCTL = 1u << 8,
    PSP_NETWORK_COMMAND_UNWIND_ONE_RUNG = 1u << 9,
    PSP_NETWORK_COMMAND_RECORD_LEASE_WEDGE = 1u << 10
};

typedef struct {
    PspNetworkSupervisor next;
    uint32_t commands;
    bool handled;
    bool deliberate_noop;
} PspNetworkSupervisorDecision;

typedef enum {
    PSP_NETWORK_PUMP_BACKGROUND = 0,
    PSP_NETWORK_PUMP_DEMAND,
    PSP_NETWORK_PUMP_SUSPEND
} PspNetworkPumpUrgency;

enum {
    PSP_NETWORK_PUMP_UNITS_BACKGROUND = 1,
    PSP_NETWORK_PUMP_UNITS_DEMAND = 12,
    PSP_NETWORK_PUMP_UNITS_SUSPEND = 24
};

enum {
    PSP_NETWORK_SUPERVISOR_VIOLATION_NONE = 0,
    PSP_NETWORK_SUPERVISOR_VIOLATION_STATE = 1u << 0,
    PSP_NETWORK_SUPERVISOR_VIOLATION_STACK = 1u << 1,
    PSP_NETWORK_SUPERVISOR_VIOLATION_CHILD = 1u << 2,
    PSP_NETWORK_SUPERVISOR_VIOLATION_ADMISSION = 1u << 3,
    PSP_NETWORK_SUPERVISOR_VIOLATION_TARGET = 1u << 4,
    PSP_NETWORK_SUPERVISOR_VIOLATION_LEASE = 1u << 5
};

PspNetworkRequestTable psp_network_request_table_initial(void);
bool psp_network_request_set(PspNetworkRequestTable *table,
                             PspNetworkRequester requester,
                             uint64_t generation, int profile_index);
bool psp_network_request_clear(PspNetworkRequestTable *table,
                               PspNetworkRequester requester,
                               uint64_t generation);
PspNetworkTarget psp_network_request_target(
    const PspNetworkRequestTable *table);

PspNetworkSupervisor psp_network_supervisor_initial(void);
PspNetworkSupervisorDecision psp_network_supervisor_transition(
    const PspNetworkSupervisor *machine,
    const PspNetworkSnapshot *snapshot,
    const PspNetworkSupervisorEvent *event);
uint32_t psp_network_supervisor_violations(
    const PspNetworkSupervisor *machine);
unsigned psp_network_supervisor_pump_units(PspNetworkPumpUrgency urgency);
const char *psp_network_supervisor_state_name(
    PspNetworkSupervisorState state);
const char *psp_network_supervisor_event_name(
    PspNetworkSupervisorEventType event);

#endif
