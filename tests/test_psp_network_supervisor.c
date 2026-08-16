#include "tilefinch/psp_network_supervisor.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "PSP NETWORK SUPERVISOR CHECK failed at %s:%d: %s\n",\
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

static PspNetworkSnapshot snapshot_for(
    PspNetworkStackState stack, uint16_t leases)
{
    PspNetworkSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.stack = stack;
    snapshot.lease_count = leases;
    snapshot.wlan_switch_on = true;
    snapshot.apctl = stack == PSP_NETWORK_STACK_FULL
        || stack == PSP_NETWORK_STACK_RETAINED_WEDGED
        ? PSP_NETWORK_APCTL_GOT_IP : PSP_NETWORK_APCTL_DISCONNECTED;
    snapshot.ladder = stack == PSP_NETWORK_STACK_FULL
        ? PSP_NETWORK_LADDER_READY : PSP_NETWORK_LADDER_DOWN;
    return snapshot;
}

static bool apply(PspNetworkSupervisor *machine,
                  PspNetworkSnapshot snapshot,
                  PspNetworkSupervisorEvent event)
{
    PspNetworkSupervisorDecision decision =
        psp_network_supervisor_transition(machine, &snapshot, &event);
    CHECK(decision.handled);
    CHECK(psp_network_supervisor_violations(&decision.next) == 0);
    *machine = decision.next;
    return true;
}

static PspNetworkTarget target(PspNetworkTargetKind kind,
                               PspNetworkTargetCause cause, int profile)
{
    PspNetworkTarget value = {kind, cause, profile};
    return value;
}

static bool test_request_precedence_and_generations(void)
{
    PspNetworkRequestTable table = psp_network_request_table_initial();
    CHECK(psp_network_request_set(
        &table, PSP_NETWORK_REQUEST_BOOT, 1, 2));
    PspNetworkTarget resolved = psp_network_request_target(&table);
    CHECK(resolved.kind == PSP_NETWORK_TARGET_READY);
    CHECK(resolved.cause == PSP_NETWORK_TARGET_CAUSE_BOOT);
    CHECK(resolved.profile_index == 2);

    CHECK(psp_network_request_set(
        &table, PSP_NETWORK_REQUEST_NAVIGATION, 7, 4));
    resolved = psp_network_request_target(&table);
    CHECK(resolved.cause == PSP_NETWORK_TARGET_CAUSE_NAVIGATION);
    CHECK(resolved.profile_index == 4);

    CHECK(psp_network_request_set(
        &table, PSP_NETWORK_REQUEST_VOICE_INHIBIT, 3, 0));
    CHECK(psp_network_request_target(&table).cause
          == PSP_NETWORK_TARGET_CAUSE_VOICE);
    CHECK(psp_network_request_set(
        &table, PSP_NETWORK_REQUEST_SUSPEND_INHIBIT, 5, 0));
    CHECK(psp_network_request_target(&table).kind
          == PSP_NETWORK_TARGET_SUSPEND);
    CHECK(psp_network_request_set(
        &table, PSP_NETWORK_REQUEST_SHUTDOWN_INHIBIT, 9, 0));
    CHECK(psp_network_request_target(&table).cause
          == PSP_NETWORK_TARGET_CAUSE_SHUTDOWN);

    CHECK(!psp_network_request_clear(
        &table, PSP_NETWORK_REQUEST_SHUTDOWN_INHIBIT, 8));
    CHECK(psp_network_request_target(&table).cause
          == PSP_NETWORK_TARGET_CAUSE_SHUTDOWN);
    CHECK(psp_network_request_clear(
        &table, PSP_NETWORK_REQUEST_SHUTDOWN_INHIBIT, 9));
    CHECK(psp_network_request_target(&table).kind
          == PSP_NETWORK_TARGET_SUSPEND);
    CHECK(!psp_network_request_set(
        &table, PSP_NETWORK_REQUEST_NAVIGATION, 6, 1));
    CHECK(table.requests[PSP_NETWORK_REQUEST_NAVIGATION].profile_index == 4);
    return true;
}

static bool start_to_ready(PspNetworkSupervisor *machine)
{
    CHECK(apply(machine, snapshot_for(PSP_NETWORK_STACK_NONE, 0),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_TARGET_CHANGED,
                    .target = target(PSP_NETWORK_TARGET_READY,
                                     PSP_NETWORK_TARGET_CAUSE_NAVIGATION, 1)
                }));
    CHECK(machine->state == PSP_NETWORK_SUPERVISOR_STARTING);
    machine->stack = PSP_NETWORK_STACK_FULL;
    CHECK(apply(machine, snapshot_for(PSP_NETWORK_STACK_FULL, 0),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_LADDER_READY
                }));
    CHECK(machine->state == PSP_NETWORK_SUPERVISOR_READY);
    CHECK(machine->admission_open);
    return true;
}

static bool test_retained_suspend_and_resume(void)
{
    PspNetworkSupervisor machine = psp_network_supervisor_initial();
    CHECK(start_to_ready(&machine));
    CHECK(apply(&machine, snapshot_for(PSP_NETWORK_STACK_FULL, 1),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_TARGET_CHANGED,
                    .target = target(PSP_NETWORK_TARGET_SUSPEND,
                                     PSP_NETWORK_TARGET_CAUSE_SUSPEND, 0)
                }));
    CHECK(machine.state == PSP_NETWORK_SUPERVISOR_STOPPING);
    CHECK(machine.stopping_target
          == PSP_NETWORK_STOP_TARGET_SUSPENDED_RETAINED);
    CHECK(apply(&machine, snapshot_for(PSP_NETWORK_STACK_FULL, 1),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_ADMISSION_STOPPED
                }));
    CHECK(machine.stopping_phase == PSP_NETWORK_STOP_DRAIN_LEASES);
    CHECK(apply(&machine, snapshot_for(PSP_NETWORK_STACK_FULL, 0),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_LEASES_RELEASED
                }));
    CHECK(machine.state == PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED);
    CHECK(machine.stack == PSP_NETWORK_STACK_FULL);

    machine.target = target(PSP_NETWORK_TARGET_READY,
                            PSP_NETWORK_TARGET_CAUSE_NAVIGATION, 1);
    CHECK(apply(&machine, snapshot_for(PSP_NETWORK_STACK_FULL, 0),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_RESUME
                }));
    CHECK(machine.state == PSP_NETWORK_SUPERVISOR_REJOINING);
    CHECK(machine.rejoin_phase == PSP_NETWORK_REJOIN_PROBING);
    CHECK(apply(&machine, snapshot_for(PSP_NETWORK_STACK_FULL, 0),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_PROBE_HEALTHY
                }));
    CHECK(machine.state == PSP_NETWORK_SUPERVISOR_READY);
    return true;
}

static bool test_suspend_timeout_never_unloads_live_lease(void)
{
    PspNetworkSupervisor machine = psp_network_supervisor_initial();
    CHECK(start_to_ready(&machine));
    CHECK(apply(&machine, snapshot_for(PSP_NETWORK_STACK_FULL, 1),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_TARGET_CHANGED,
                    .target = target(PSP_NETWORK_TARGET_SUSPEND,
                                     PSP_NETWORK_TARGET_CAUSE_SUSPEND, 0)
                }));
    CHECK(apply(&machine, snapshot_for(PSP_NETWORK_STACK_FULL, 1),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_ADMISSION_STOPPED
                }));
    PspNetworkSupervisorDecision decision =
        psp_network_supervisor_transition(
            &machine, &(PspNetworkSnapshot) {
                .stack = PSP_NETWORK_STACK_FULL,
                .lease_count = 1,
                .apctl = PSP_NETWORK_APCTL_GOT_IP,
                .wlan_switch_on = true
            }, &(PspNetworkSupervisorEvent) {
                .type = PSP_NETWORK_EVENT_DRAIN_TIMEOUT
            });
    CHECK(decision.handled);
    CHECK(decision.next.state
          == PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED);
    CHECK(decision.next.stack == PSP_NETWORK_STACK_RETAINED_WEDGED);
    CHECK(decision.next.retained_lease_count == 1);
    CHECK((decision.commands & PSP_NETWORK_COMMAND_RECORD_LEASE_WEDGE) != 0);
    CHECK((decision.commands & PSP_NETWORK_COMMAND_DISCONNECT_APCTL) == 0);
    CHECK((decision.commands & PSP_NETWORK_COMMAND_UNWIND_ONE_RUNG) == 0);
    PspNetworkSupervisor suspended = decision.next;
    suspended.target = target(PSP_NETWORK_TARGET_READY,
                              PSP_NETWORK_TARGET_CAUSE_NAVIGATION, 1);
    decision = psp_network_supervisor_transition(
        &suspended, &(PspNetworkSnapshot) {
            .stack = PSP_NETWORK_STACK_RETAINED_WEDGED,
            .lease_count = 1,
            .apctl = PSP_NETWORK_APCTL_GOT_IP,
            .wlan_switch_on = true
        }, &(PspNetworkSupervisorEvent) {
            .type = PSP_NETWORK_EVENT_RESUME
        });
    CHECK(decision.next.state
          == PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED);
    CHECK(decision.deliberate_noop);
    CHECK((decision.commands & PSP_NETWORK_COMMAND_OPEN_ADMISSION) == 0);
    CHECK(apply(&suspended,
                snapshot_for(PSP_NETWORK_STACK_RETAINED_WEDGED, 0),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_LEASES_RELEASED
                }));
    CHECK(suspended.state == PSP_NETWORK_SUPERVISOR_REJOINING);
    return true;
}

static bool test_off_timeout_quarantines_until_release(void)
{
    PspNetworkSupervisor machine = psp_network_supervisor_initial();
    CHECK(start_to_ready(&machine));
    CHECK(apply(&machine, snapshot_for(PSP_NETWORK_STACK_FULL, 2),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_TARGET_CHANGED,
                    .target = target(PSP_NETWORK_TARGET_OFF,
                                     PSP_NETWORK_TARGET_CAUSE_VOICE, 0)
                }));
    CHECK(apply(&machine, snapshot_for(PSP_NETWORK_STACK_FULL, 2),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_ADMISSION_STOPPED
                }));
    CHECK(apply(&machine, snapshot_for(PSP_NETWORK_STACK_FULL, 2),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_DRAIN_TIMEOUT
                }));
    CHECK(machine.state == PSP_NETWORK_SUPERVISOR_STOPPING);
    CHECK(machine.stopping_phase == PSP_NETWORK_STOP_LEASE_WEDGED);
    CHECK(machine.stack == PSP_NETWORK_STACK_RETAINED_WEDGED);

    PspNetworkSupervisorDecision ignored =
        psp_network_supervisor_transition(
            &machine, &(PspNetworkSnapshot) {
                .stack = PSP_NETWORK_STACK_RETAINED_WEDGED,
                .lease_count = 2
            }, &(PspNetworkSupervisorEvent) {
                .type = PSP_NETWORK_EVENT_APCTL_DISCONNECTED
            });
    CHECK(ignored.deliberate_noop);
    CHECK((ignored.commands & PSP_NETWORK_COMMAND_UNWIND_ONE_RUNG) == 0);

    CHECK(apply(&machine,
                snapshot_for(PSP_NETWORK_STACK_RETAINED_WEDGED, 0),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_LEASES_RELEASED
                }));
    CHECK(machine.stopping_phase == PSP_NETWORK_STOP_LEAVE_APCTL);
    return true;
}

static bool test_rejoin_escalates_through_full_restart(void)
{
    PspNetworkSupervisor machine = psp_network_supervisor_initial();
    CHECK(start_to_ready(&machine));
    CHECK(apply(&machine, snapshot_for(PSP_NETWORK_STACK_FULL, 0),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_REGRESSION_HINT
                }));
    CHECK(machine.state == PSP_NETWORK_SUPERVISOR_REJOINING);
    CHECK(apply(&machine, snapshot_for(PSP_NETWORK_STACK_FULL, 0),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_PROBE_FAILED
                }));
    CHECK(machine.rejoin_phase == PSP_NETWORK_REJOIN_APCTL);
    CHECK(apply(&machine, snapshot_for(PSP_NETWORK_STACK_FULL, 0),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_REJOIN_FAILED
                }));
    CHECK(machine.state == PSP_NETWORK_SUPERVISOR_STOPPING);
    CHECK(machine.stopping_target == PSP_NETWORK_STOP_TARGET_RESTART);
    CHECK(machine.rejoin_attempts == 2);
    return true;
}

static bool test_cancelled_start_retains_honest_failure(void)
{
    PspNetworkSupervisor machine = psp_network_supervisor_initial();
    CHECK(apply(&machine, snapshot_for(PSP_NETWORK_STACK_NONE, 0),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_TARGET_CHANGED,
                    .target = target(PSP_NETWORK_TARGET_READY,
                                     PSP_NETWORK_TARGET_CAUSE_NAVIGATION, 1)
                }));
    CHECK(machine.state == PSP_NETWORK_SUPERVISOR_STARTING);
    CHECK(apply(&machine, snapshot_for(PSP_NETWORK_STACK_PARTIAL, 0),
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_LADDER_FAILED,
                    .failure = PSP_NETWORK_FAILURE_CANCELLED
                }));
    CHECK(machine.state == PSP_NETWORK_SUPERVISOR_STOPPING);
    CHECK(machine.stopping_target == PSP_NETWORK_STOP_TARGET_OFFLINE);
    CHECK(machine.failure == PSP_NETWORK_FAILURE_CANCELLED);
    return true;
}

static PspNetworkSupervisor canonical_machine(
    PspNetworkSupervisorState state)
{
    PspNetworkSupervisor machine = psp_network_supervisor_initial();
    machine.state = state;
    machine.target = target(PSP_NETWORK_TARGET_READY,
                            PSP_NETWORK_TARGET_CAUSE_NAVIGATION, 1);
    switch (state) {
    case PSP_NETWORK_SUPERVISOR_STARTING:
        machine.stack = PSP_NETWORK_STACK_PARTIAL;
        break;
    case PSP_NETWORK_SUPERVISOR_READY:
        machine.stack = PSP_NETWORK_STACK_FULL;
        machine.admission_open = true;
        break;
    case PSP_NETWORK_SUPERVISOR_REJOINING:
        machine.stack = PSP_NETWORK_STACK_FULL;
        machine.rejoin_phase = PSP_NETWORK_REJOIN_PROBING;
        break;
    case PSP_NETWORK_SUPERVISOR_STOPPING:
        machine.stack = PSP_NETWORK_STACK_FULL;
        machine.stopping_phase = PSP_NETWORK_STOP_DRAIN_LEASES;
        machine.stopping_target = PSP_NETWORK_STOP_TARGET_OFF;
        break;
    case PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED:
        machine.stack = PSP_NETWORK_STACK_FULL;
        break;
    case PSP_NETWORK_SUPERVISOR_SUSPENDED_OFF:
    case PSP_NETWORK_SUPERVISOR_OFFLINE:
    case PSP_NETWORK_SUPERVISOR_OFF:
    default:
        machine.stack = PSP_NETWORK_STACK_NONE;
        break;
    }
    return machine;
}

static bool test_event_state_grid_is_total(void)
{
    for (int state = PSP_NETWORK_SUPERVISOR_OFF;
         state < PSP_NETWORK_SUPERVISOR_STATE_COUNT; state++) {
        PspNetworkSupervisor machine = canonical_machine(
            (PspNetworkSupervisorState)state);
        CHECK(psp_network_supervisor_violations(&machine) == 0);
        for (int type = PSP_NETWORK_EVENT_TARGET_CHANGED;
             type < PSP_NETWORK_EVENT_COUNT; type++) {
            PspNetworkSnapshot snapshot =
                snapshot_for(machine.stack, state == PSP_NETWORK_SUPERVISOR_STOPPING ? 1 : 0);
            PspNetworkSupervisorEvent event;
            memset(&event, 0, sizeof(event));
            event.type = (PspNetworkSupervisorEventType)type;
            event.target = target(PSP_NETWORK_TARGET_READY,
                                  PSP_NETWORK_TARGET_CAUSE_NAVIGATION, 1);
            PspNetworkSupervisorDecision decision =
                psp_network_supervisor_transition(&machine, &snapshot, &event);
            CHECK(decision.handled);
            CHECK(psp_network_supervisor_violations(&decision.next) == 0);
            CHECK(decision.deliberate_noop
                  || decision.commands != 0
                  || memcmp(&decision.next, &machine, sizeof(machine)) != 0);
        }
    }
    return true;
}

static bool test_demand_scaled_pump_policy(void)
{
    CHECK(psp_network_supervisor_pump_units(PSP_NETWORK_PUMP_BACKGROUND)
          == 1);
    CHECK(psp_network_supervisor_pump_units(PSP_NETWORK_PUMP_DEMAND)
          > psp_network_supervisor_pump_units(PSP_NETWORK_PUMP_BACKGROUND));
    CHECK(psp_network_supervisor_pump_units(PSP_NETWORK_PUMP_SUSPEND)
          > psp_network_supervisor_pump_units(PSP_NETWORK_PUMP_DEMAND));
    CHECK(psp_network_supervisor_pump_units(PSP_NETWORK_PUMP_SUSPEND) <= 32);
    return true;
}

int main(void)
{
    if (!test_request_precedence_and_generations()
        || !test_retained_suspend_and_resume()
        || !test_suspend_timeout_never_unloads_live_lease()
        || !test_off_timeout_quarantines_until_release()
        || !test_rejoin_escalates_through_full_restart()
        || !test_cancelled_start_retains_honest_failure()
        || !test_event_state_grid_is_total()
        || !test_demand_scaled_pump_policy())
        return 1;
    puts("psp network supervisor tests passed");
    return 0;
}
