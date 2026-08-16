#include "tilefinch/psp_network_supervisor.h"

#include <string.h>

static PspNetworkTarget psp_network_target(
    PspNetworkTargetKind kind, PspNetworkTargetCause cause, int profile)
{
    PspNetworkTarget target;
    target.kind = kind;
    target.cause = cause;
    target.profile_index = profile;
    return target;
}

PspNetworkRequestTable psp_network_request_table_initial(void)
{
    PspNetworkRequestTable table;
    memset(&table, 0, sizeof(table));
    return table;
}

bool psp_network_request_set(PspNetworkRequestTable *table,
                             PspNetworkRequester requester,
                             uint64_t generation, int profile_index)
{
    if (table == NULL || requester < PSP_NETWORK_REQUEST_BOOT
        || requester >= PSP_NETWORK_REQUEST_COUNT)
        return false;
    PspNetworkRequest *request = &table->requests[requester];
    if (generation < request->generation) return false;
    request->active = true;
    request->generation = generation;
    request->profile_index = profile_index;
    return true;
}

bool psp_network_request_clear(PspNetworkRequestTable *table,
                               PspNetworkRequester requester,
                               uint64_t generation)
{
    if (table == NULL || requester < PSP_NETWORK_REQUEST_BOOT
        || requester >= PSP_NETWORK_REQUEST_COUNT)
        return false;
    PspNetworkRequest *request = &table->requests[requester];
    if (generation != request->generation) return false;
    request->active = false;
    return true;
}

PspNetworkTarget psp_network_request_target(
    const PspNetworkRequestTable *table)
{
    if (table == NULL)
        return psp_network_target(PSP_NETWORK_TARGET_OFF,
                                  PSP_NETWORK_TARGET_CAUSE_NONE, 0);
    if (table->requests[PSP_NETWORK_REQUEST_SHUTDOWN_INHIBIT].active)
        return psp_network_target(PSP_NETWORK_TARGET_OFF,
                                  PSP_NETWORK_TARGET_CAUSE_SHUTDOWN, 0);
    if (table->requests[PSP_NETWORK_REQUEST_SUSPEND_INHIBIT].active)
        return psp_network_target(PSP_NETWORK_TARGET_SUSPEND,
                                  PSP_NETWORK_TARGET_CAUSE_SUSPEND, 0);
    if (table->requests[PSP_NETWORK_REQUEST_VOICE_INHIBIT].active)
        return psp_network_target(PSP_NETWORK_TARGET_OFF,
                                  PSP_NETWORK_TARGET_CAUSE_VOICE, 0);
    if (table->requests[PSP_NETWORK_REQUEST_NAVIGATION].active) {
        const PspNetworkRequest *request =
            &table->requests[PSP_NETWORK_REQUEST_NAVIGATION];
        return psp_network_target(PSP_NETWORK_TARGET_READY,
                                  PSP_NETWORK_TARGET_CAUSE_NAVIGATION,
                                  request->profile_index);
    }
    if (table->requests[PSP_NETWORK_REQUEST_BOOT].active) {
        const PspNetworkRequest *request =
            &table->requests[PSP_NETWORK_REQUEST_BOOT];
        return psp_network_target(PSP_NETWORK_TARGET_READY,
                                  PSP_NETWORK_TARGET_CAUSE_BOOT,
                                  request->profile_index);
    }
    return psp_network_target(PSP_NETWORK_TARGET_OFF,
                              PSP_NETWORK_TARGET_CAUSE_NONE, 0);
}

static void psp_network_clear_children(PspNetworkSupervisor *machine)
{
    machine->rejoin_phase = PSP_NETWORK_REJOIN_NONE;
    machine->stopping_phase = PSP_NETWORK_STOP_NONE;
    machine->stopping_target = PSP_NETWORK_STOP_TARGET_NONE;
}

static void psp_network_enter_starting(PspNetworkSupervisorDecision *decision)
{
    psp_network_clear_children(&decision->next);
    decision->next.state = PSP_NETWORK_SUPERVISOR_STARTING;
    decision->next.stack = PSP_NETWORK_STACK_NONE;
    decision->next.admission_open = false;
    decision->next.retained_lease_count = 0;
    decision->commands |= PSP_NETWORK_COMMAND_START_LADDER;
}

static void psp_network_enter_ready(PspNetworkSupervisorDecision *decision)
{
    psp_network_clear_children(&decision->next);
    decision->next.state = PSP_NETWORK_SUPERVISOR_READY;
    decision->next.stack = PSP_NETWORK_STACK_FULL;
    decision->next.admission_open = true;
    decision->next.failure = PSP_NETWORK_FAILURE_NONE;
    decision->next.rejoin_attempts = 0;
    decision->next.retained_lease_count = 0;
    decision->commands |= PSP_NETWORK_COMMAND_OPEN_ADMISSION;
}

static void psp_network_enter_rejoining(
    PspNetworkSupervisorDecision *decision)
{
    psp_network_clear_children(&decision->next);
    decision->next.state = PSP_NETWORK_SUPERVISOR_REJOINING;
    decision->next.rejoin_phase = PSP_NETWORK_REJOIN_PROBING;
    decision->next.stack = PSP_NETWORK_STACK_FULL;
    decision->next.admission_open = false;
    decision->commands |= PSP_NETWORK_COMMAND_STOP_ADMISSION
        | PSP_NETWORK_COMMAND_PROBE_LINK;
}

static void psp_network_enter_stopping(
    PspNetworkSupervisorDecision *decision,
    PspNetworkStoppingTarget target,
    PspNetworkFailureKind failure)
{
    psp_network_clear_children(&decision->next);
    decision->next.state = PSP_NETWORK_SUPERVISOR_STOPPING;
    decision->next.stopping_phase = PSP_NETWORK_STOP_ADMISSION;
    decision->next.stopping_target = target;
    decision->next.admission_open = false;
    if (failure != PSP_NETWORK_FAILURE_NONE)
        decision->next.failure = failure;
    decision->commands |= PSP_NETWORK_COMMAND_STOP_ADMISSION;
}

static void psp_network_finish_stopping(
    PspNetworkSupervisorDecision *decision)
{
    PspNetworkStoppingTarget target = decision->next.stopping_target;
    PspNetworkTarget desired = decision->next.target;
    PspNetworkFailureKind failure = decision->next.failure;
    psp_network_clear_children(&decision->next);
    decision->next.stack = PSP_NETWORK_STACK_NONE;
    decision->next.admission_open = false;
    decision->next.retained_lease_count = 0;
    switch (target) {
    case PSP_NETWORK_STOP_TARGET_OFF:
        decision->next.state = PSP_NETWORK_SUPERVISOR_OFF;
        decision->next.failure = PSP_NETWORK_FAILURE_NONE;
        break;
    case PSP_NETWORK_STOP_TARGET_OFFLINE:
        decision->next.state = PSP_NETWORK_SUPERVISOR_OFFLINE;
        decision->next.failure = failure == PSP_NETWORK_FAILURE_NONE
            ? PSP_NETWORK_FAILURE_START : failure;
        break;
    case PSP_NETWORK_STOP_TARGET_SUSPENDED_OFF:
        decision->next.state = PSP_NETWORK_SUPERVISOR_SUSPENDED_OFF;
        break;
    case PSP_NETWORK_STOP_TARGET_RESTART:
        decision->next.target = desired;
        psp_network_enter_starting(decision);
        break;
    default:
        decision->next.state = PSP_NETWORK_SUPERVISOR_OFFLINE;
        decision->next.failure = PSP_NETWORK_FAILURE_START;
        break;
    }
}

static void psp_network_begin_leave_or_finish(
    PspNetworkSupervisorDecision *decision)
{
    if (decision->next.stopping_target
        == PSP_NETWORK_STOP_TARGET_SUSPENDED_RETAINED) {
        psp_network_clear_children(&decision->next);
        decision->next.state = PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED;
        decision->next.stack = PSP_NETWORK_STACK_FULL;
        decision->next.admission_open = false;
        decision->next.retained_lease_count = 0;
        return;
    }
    if (decision->next.stack == PSP_NETWORK_STACK_NONE) {
        psp_network_finish_stopping(decision);
        return;
    }
    decision->next.stopping_phase = PSP_NETWORK_STOP_LEAVE_APCTL;
    decision->commands |= PSP_NETWORK_COMMAND_DISCONNECT_APCTL;
}

PspNetworkSupervisor psp_network_supervisor_initial(void)
{
    PspNetworkSupervisor machine;
    memset(&machine, 0, sizeof(machine));
    machine.state = PSP_NETWORK_SUPERVISOR_OFF;
    machine.stack = PSP_NETWORK_STACK_NONE;
    machine.target = psp_network_target(PSP_NETWORK_TARGET_OFF,
                                        PSP_NETWORK_TARGET_CAUSE_NONE, 0);
    machine.rejoin_budget = 2;
    return machine;
}

static void psp_network_apply_target(
    PspNetworkSupervisorDecision *decision,
    const PspNetworkSnapshot *snapshot,
    PspNetworkTarget target)
{
    decision->next.target = target;
    if (decision->next.state == PSP_NETWORK_SUPERVISOR_STOPPING) {
        if (target.kind == PSP_NETWORK_TARGET_READY)
            decision->next.stopping_target = PSP_NETWORK_STOP_TARGET_RESTART;
        else if (target.kind == PSP_NETWORK_TARGET_SUSPEND)
            decision->next.stopping_target =
                decision->next.stack == PSP_NETWORK_STACK_FULL
                || decision->next.stack == PSP_NETWORK_STACK_RETAINED_WEDGED
                    ? PSP_NETWORK_STOP_TARGET_SUSPENDED_RETAINED
                    : PSP_NETWORK_STOP_TARGET_SUSPENDED_OFF;
        else
            decision->next.stopping_target = PSP_NETWORK_STOP_TARGET_OFF;
        return;
    }

    if (target.kind == PSP_NETWORK_TARGET_READY) {
        switch (decision->next.state) {
        case PSP_NETWORK_SUPERVISOR_OFF:
        case PSP_NETWORK_SUPERVISOR_OFFLINE:
        case PSP_NETWORK_SUPERVISOR_SUSPENDED_OFF:
            psp_network_enter_starting(decision);
            break;
        case PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED:
            if (decision->next.stack
                == PSP_NETWORK_STACK_RETAINED_WEDGED) {
                /* The old consumer must publish its lease release before
                   admission can reopen or the retained stack can be probed. */
                decision->deliberate_noop = true;
            } else {
                psp_network_enter_rejoining(decision);
            }
            break;
        default:
            decision->deliberate_noop = true;
            break;
        }
        return;
    }

    if (target.kind == PSP_NETWORK_TARGET_SUSPEND) {
        switch (decision->next.state) {
        case PSP_NETWORK_SUPERVISOR_OFF:
        case PSP_NETWORK_SUPERVISOR_OFFLINE:
        case PSP_NETWORK_SUPERVISOR_SUSPENDED_OFF:
            psp_network_clear_children(&decision->next);
            decision->next.state = PSP_NETWORK_SUPERVISOR_SUSPENDED_OFF;
            decision->next.stack = PSP_NETWORK_STACK_NONE;
            decision->next.admission_open = false;
            break;
        case PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED:
            decision->deliberate_noop = true;
            break;
        case PSP_NETWORK_SUPERVISOR_READY:
        case PSP_NETWORK_SUPERVISOR_REJOINING:
            psp_network_enter_stopping(
                decision, PSP_NETWORK_STOP_TARGET_SUSPENDED_RETAINED,
                PSP_NETWORK_FAILURE_NONE);
            break;
        case PSP_NETWORK_SUPERVISOR_STARTING:
            /* The inner ladder owns the partial-rung truth while Starting. */
            decision->next.stack = snapshot->stack;
            psp_network_enter_stopping(
                decision, PSP_NETWORK_STOP_TARGET_SUSPENDED_OFF,
                PSP_NETWORK_FAILURE_NONE);
            break;
        default:
            decision->deliberate_noop = true;
            break;
        }
        return;
    }

    switch (decision->next.state) {
    case PSP_NETWORK_SUPERVISOR_OFF:
        decision->deliberate_noop = true;
        break;
    case PSP_NETWORK_SUPERVISOR_OFFLINE:
    case PSP_NETWORK_SUPERVISOR_SUSPENDED_OFF:
        psp_network_clear_children(&decision->next);
        decision->next.state = PSP_NETWORK_SUPERVISOR_OFF;
        decision->next.stack = PSP_NETWORK_STACK_NONE;
        decision->next.failure = PSP_NETWORK_FAILURE_NONE;
        break;
    case PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED:
    case PSP_NETWORK_SUPERVISOR_READY:
    case PSP_NETWORK_SUPERVISOR_REJOINING:
    case PSP_NETWORK_SUPERVISOR_STARTING:
        if (decision->next.state == PSP_NETWORK_SUPERVISOR_STARTING)
            decision->next.stack = snapshot->stack;
        psp_network_enter_stopping(
            decision, PSP_NETWORK_STOP_TARGET_OFF,
            PSP_NETWORK_FAILURE_NONE);
        break;
    default:
        (void)snapshot;
        decision->deliberate_noop = true;
        break;
    }
}

static void psp_network_transition_stopping(
    PspNetworkSupervisorDecision *decision,
    const PspNetworkSnapshot *snapshot,
    const PspNetworkSupervisorEvent *event)
{
    switch (decision->next.stopping_phase) {
    case PSP_NETWORK_STOP_ADMISSION:
        if (event->type != PSP_NETWORK_EVENT_ADMISSION_STOPPED) return;
        if (snapshot->lease_count != 0) {
            decision->next.stopping_phase = PSP_NETWORK_STOP_DRAIN_LEASES;
            decision->commands |= PSP_NETWORK_COMMAND_CANCEL_CONSUMERS;
        } else {
            psp_network_begin_leave_or_finish(decision);
        }
        break;
    case PSP_NETWORK_STOP_DRAIN_LEASES:
        if (event->type == PSP_NETWORK_EVENT_LEASES_RELEASED) {
            psp_network_begin_leave_or_finish(decision);
        } else if (event->type == PSP_NETWORK_EVENT_DRAIN_TIMEOUT) {
            decision->next.retained_lease_count = snapshot->lease_count;
            decision->commands |= PSP_NETWORK_COMMAND_RECORD_LEASE_WEDGE;
            if (decision->next.stopping_target
                == PSP_NETWORK_STOP_TARGET_SUSPENDED_RETAINED) {
                psp_network_clear_children(&decision->next);
                decision->next.state =
                    PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED;
                decision->next.stack = PSP_NETWORK_STACK_RETAINED_WEDGED;
            } else {
                decision->next.stopping_phase =
                    PSP_NETWORK_STOP_LEASE_WEDGED;
                decision->next.stack = PSP_NETWORK_STACK_RETAINED_WEDGED;
                decision->next.failure = PSP_NETWORK_FAILURE_LEASE_WEDGED;
            }
        }
        break;
    case PSP_NETWORK_STOP_LEASE_WEDGED:
        if (event->type == PSP_NETWORK_EVENT_LEASES_RELEASED) {
            decision->next.stack = PSP_NETWORK_STACK_FULL;
            decision->next.retained_lease_count = 0;
            psp_network_begin_leave_or_finish(decision);
        }
        break;
    case PSP_NETWORK_STOP_LEAVE_APCTL:
        if (event->type == PSP_NETWORK_EVENT_APCTL_DISCONNECTED
            || event->type == PSP_NETWORK_EVENT_LEAVE_TIMEOUT) {
            decision->next.stopping_phase = PSP_NETWORK_STOP_UNWIND_RUNGS;
            decision->commands |= PSP_NETWORK_COMMAND_UNWIND_ONE_RUNG;
        }
        break;
    case PSP_NETWORK_STOP_UNWIND_RUNGS:
        if (event->type == PSP_NETWORK_EVENT_RUNG_UNWOUND)
            decision->commands |= PSP_NETWORK_COMMAND_UNWIND_ONE_RUNG;
        else if (event->type == PSP_NETWORK_EVENT_UNWOUND)
            psp_network_finish_stopping(decision);
        break;
    case PSP_NETWORK_STOP_NONE:
    default:
        break;
    }
}

PspNetworkSupervisorDecision psp_network_supervisor_transition(
    const PspNetworkSupervisor *machine,
    const PspNetworkSnapshot *snapshot,
    const PspNetworkSupervisorEvent *event)
{
    PspNetworkSupervisorDecision decision;
    memset(&decision, 0, sizeof(decision));
    if (machine == NULL || snapshot == NULL || event == NULL
        || event->type <= PSP_NETWORK_EVENT_NONE
        || event->type >= PSP_NETWORK_EVENT_COUNT)
        return decision;
    decision.next = *machine;
    decision.handled = true;

    if (event->type == PSP_NETWORK_EVENT_TARGET_CHANGED) {
        psp_network_apply_target(&decision, snapshot, event->target);
        goto complete;
    }

    if (machine->state == PSP_NETWORK_SUPERVISOR_STOPPING) {
        psp_network_transition_stopping(&decision, snapshot, event);
        goto complete;
    }

    switch (machine->state) {
    case PSP_NETWORK_SUPERVISOR_STARTING:
        if (event->type == PSP_NETWORK_EVENT_LADDER_READY) {
            psp_network_enter_ready(&decision);
        } else if (event->type == PSP_NETWORK_EVENT_LADDER_FAILED) {
            decision.next.stack = snapshot->stack;
            psp_network_enter_stopping(
                &decision, PSP_NETWORK_STOP_TARGET_OFFLINE,
                event->failure == PSP_NETWORK_FAILURE_NONE
                    ? PSP_NETWORK_FAILURE_START : event->failure);
        }
        break;
    case PSP_NETWORK_SUPERVISOR_READY:
        if (event->type == PSP_NETWORK_EVENT_REGRESSION_HINT)
            psp_network_enter_rejoining(&decision);
        break;
    case PSP_NETWORK_SUPERVISOR_REJOINING:
        if (event->type == PSP_NETWORK_EVENT_PROBE_HEALTHY
            || event->type == PSP_NETWORK_EVENT_REJOINED) {
            psp_network_enter_ready(&decision);
        } else if (event->type == PSP_NETWORK_EVENT_PROBE_FAILED) {
            decision.next.rejoin_phase = PSP_NETWORK_REJOIN_APCTL;
            decision.next.rejoin_attempts = 1;
            decision.commands |= PSP_NETWORK_COMMAND_REJOIN_APCTL;
        } else if (event->type == PSP_NETWORK_EVENT_REJOIN_FAILED) {
            if (decision.next.rejoin_attempts
                < decision.next.rejoin_budget) {
                decision.next.rejoin_attempts++;
                psp_network_enter_stopping(
                    &decision, PSP_NETWORK_STOP_TARGET_RESTART,
                    PSP_NETWORK_FAILURE_REJOIN);
            } else {
                psp_network_enter_stopping(
                    &decision, PSP_NETWORK_STOP_TARGET_OFFLINE,
                    PSP_NETWORK_FAILURE_REJOIN);
            }
        }
        break;
    case PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED:
        if (event->type == PSP_NETWORK_EVENT_RESUME) {
            if (machine->stack == PSP_NETWORK_STACK_RETAINED_WEDGED)
                decision.deliberate_noop = true;
            else
                psp_network_enter_rejoining(&decision);
        } else if (event->type == PSP_NETWORK_EVENT_LEASES_RELEASED
                   && machine->stack
                        == PSP_NETWORK_STACK_RETAINED_WEDGED) {
            decision.next.stack = PSP_NETWORK_STACK_FULL;
            decision.next.retained_lease_count = 0;
            if (machine->target.kind == PSP_NETWORK_TARGET_READY)
                psp_network_enter_rejoining(&decision);
        }
        break;
    case PSP_NETWORK_SUPERVISOR_SUSPENDED_OFF:
        if (event->type == PSP_NETWORK_EVENT_RESUME
            && machine->target.kind == PSP_NETWORK_TARGET_READY)
            psp_network_enter_starting(&decision);
        break;
    case PSP_NETWORK_SUPERVISOR_OFFLINE:
        if (event->type == PSP_NETWORK_EVENT_RETRY
            && machine->target.kind == PSP_NETWORK_TARGET_READY)
            psp_network_enter_starting(&decision);
        break;
    case PSP_NETWORK_SUPERVISOR_OFF:
    default:
        break;
    }

complete:
    if (decision.commands == 0
        && memcmp(&decision.next, machine, sizeof(*machine)) == 0)
        decision.deliberate_noop = true;
    return decision;
}

uint32_t psp_network_supervisor_violations(
    const PspNetworkSupervisor *machine)
{
    if (machine == NULL) return UINT32_MAX;
    uint32_t result = 0;
    if (machine->state < PSP_NETWORK_SUPERVISOR_OFF
        || machine->state >= PSP_NETWORK_SUPERVISOR_STATE_COUNT)
        result |= PSP_NETWORK_SUPERVISOR_VIOLATION_STATE;

    bool stack_ok = false;
    switch (machine->state) {
    case PSP_NETWORK_SUPERVISOR_OFF:
    case PSP_NETWORK_SUPERVISOR_OFFLINE:
    case PSP_NETWORK_SUPERVISOR_SUSPENDED_OFF:
        stack_ok = machine->stack == PSP_NETWORK_STACK_NONE;
        break;
    case PSP_NETWORK_SUPERVISOR_STARTING:
    case PSP_NETWORK_SUPERVISOR_STOPPING:
        stack_ok = machine->stack >= PSP_NETWORK_STACK_NONE
            && machine->stack <= PSP_NETWORK_STACK_RETAINED_WEDGED;
        break;
    case PSP_NETWORK_SUPERVISOR_READY:
    case PSP_NETWORK_SUPERVISOR_REJOINING:
        stack_ok = machine->stack == PSP_NETWORK_STACK_FULL;
        break;
    case PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED:
        stack_ok = machine->stack == PSP_NETWORK_STACK_FULL
            || machine->stack == PSP_NETWORK_STACK_RETAINED_WEDGED;
        break;
    default:
        break;
    }
    if (!stack_ok) result |= PSP_NETWORK_SUPERVISOR_VIOLATION_STACK;

    bool stopping = machine->state == PSP_NETWORK_SUPERVISOR_STOPPING;
    if (stopping
        != (machine->stopping_phase != PSP_NETWORK_STOP_NONE
            && machine->stopping_target != PSP_NETWORK_STOP_TARGET_NONE))
        result |= PSP_NETWORK_SUPERVISOR_VIOLATION_CHILD;
    if ((machine->state == PSP_NETWORK_SUPERVISOR_REJOINING)
        != (machine->rejoin_phase != PSP_NETWORK_REJOIN_NONE))
        result |= PSP_NETWORK_SUPERVISOR_VIOLATION_CHILD;
    if (machine->admission_open
        != (machine->state == PSP_NETWORK_SUPERVISOR_READY))
        result |= PSP_NETWORK_SUPERVISOR_VIOLATION_ADMISSION;
    if (machine->target.kind < PSP_NETWORK_TARGET_OFF
        || machine->target.kind > PSP_NETWORK_TARGET_SUSPEND)
        result |= PSP_NETWORK_SUPERVISOR_VIOLATION_TARGET;
    if (machine->retained_lease_count != 0
        && machine->stack != PSP_NETWORK_STACK_RETAINED_WEDGED)
        result |= PSP_NETWORK_SUPERVISOR_VIOLATION_LEASE;
    if (machine->stack == PSP_NETWORK_STACK_RETAINED_WEDGED
        && machine->state != PSP_NETWORK_SUPERVISOR_STOPPING
        && machine->state != PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED)
        result |= PSP_NETWORK_SUPERVISOR_VIOLATION_LEASE;
    return result;
}

unsigned psp_network_supervisor_pump_units(PspNetworkPumpUrgency urgency)
{
    switch (urgency) {
    case PSP_NETWORK_PUMP_DEMAND:
        return PSP_NETWORK_PUMP_UNITS_DEMAND;
    case PSP_NETWORK_PUMP_SUSPEND:
        return PSP_NETWORK_PUMP_UNITS_SUSPEND;
    case PSP_NETWORK_PUMP_BACKGROUND:
    default:
        return PSP_NETWORK_PUMP_UNITS_BACKGROUND;
    }
}

const char *psp_network_supervisor_state_name(
    PspNetworkSupervisorState state)
{
    static const char *const names[] = {
        "off", "starting", "ready", "rejoining", "stopping",
        "suspended-retained", "suspended-off", "offline"
    };
    return state >= PSP_NETWORK_SUPERVISOR_OFF
        && state < PSP_NETWORK_SUPERVISOR_STATE_COUNT
        ? names[state] : "invalid";
}

const char *psp_network_supervisor_event_name(
    PspNetworkSupervisorEventType event)
{
    static const char *const names[] = {
        "none", "target-changed", "resume", "ladder-ready",
        "ladder-failed", "regression-hint", "probe-healthy",
        "probe-failed", "rejoined", "rejoin-failed",
        "admission-stopped", "leases-released", "drain-timeout",
        "apctl-disconnected", "leave-timeout", "rung-unwound",
        "unwound", "retry"
    };
    return event >= PSP_NETWORK_EVENT_NONE
        && event < PSP_NETWORK_EVENT_COUNT ? names[event] : "invalid";
}
