/* Live-network association, supervision, reporting, and shutdown for the PSP
 * browser. The whole file is inside TILEFINCH_PSP_LIVE_NETWORK: replay builds
 * compile it away and link an empty object.
 */
#include "psp_app_internal.h"

#ifdef TILEFINCH_PSP_LIVE_NETWORK
static PspNetworkLifecycle *psp_bound_network_lifecycle;
static bool psp_network_lifecycle_pump_stopping(
    PspNetworkLifecycle *lifecycle, PspNetwork *network,
    uint64_t now_us);

static PspNetworkStackState psp_network_lifecycle_stack(
    const PspNetwork *network)
{
    if (network == NULL) return PSP_NETWORK_STACK_NONE;
    if (network->core_initialized && network->inet_initialized
        && network->resolver_initialized && network->apctl_initialized)
        return PSP_NETWORK_STACK_FULL;
    if (network->common_loaded || network->inet_loaded
        || network->core_initialized || network->inet_initialized
        || network->resolver_initialized || network->apctl_initialized
        || network->connect_started)
        return PSP_NETWORK_STACK_PARTIAL;
    return PSP_NETWORK_STACK_NONE;
}

static PspNetworkSnapshot psp_network_lifecycle_snapshot(
    const PspNetwork *network)
{
    PspNetworkSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    size_t active_operations =
        fetch_background_transport_active_operations();
    snapshot.lease_count = active_operations > UINT16_MAX
        ? UINT16_MAX : (uint16_t) active_operations;
    snapshot.stack = psp_network_lifecycle_stack(network);
    snapshot.wlan_switch_on = network == NULL
        || network->wlan_switch_state != 0;
    if (network == NULL) {
        snapshot.apctl = PSP_NETWORK_APCTL_UNKNOWN;
        snapshot.ladder = PSP_NETWORK_LADDER_DOWN;
    } else if (network->status == PSP_NETWORK_READY) {
        snapshot.apctl = PSP_NETWORK_APCTL_GOT_IP;
        snapshot.ladder = PSP_NETWORK_LADDER_READY;
    } else if (psp_network_status_active(network->status)) {
        snapshot.apctl = network->connect_started
            ? PSP_NETWORK_APCTL_ASSOCIATING
            : PSP_NETWORK_APCTL_DISCONNECTED;
        snapshot.ladder = PSP_NETWORK_LADDER_RUNNING;
    } else if (network->status == PSP_NETWORK_FAILED
               || network->status == PSP_NETWORK_CANCELLED) {
        snapshot.apctl = PSP_NETWORK_APCTL_DISCONNECTED;
        snapshot.ladder = PSP_NETWORK_LADDER_FAILED;
    } else {
        snapshot.apctl = PSP_NETWORK_APCTL_DISCONNECTED;
        snapshot.ladder = PSP_NETWORK_LADDER_DOWN;
    }
    return snapshot;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
static void psp_network_lifecycle_trace(
    PspNetworkLifecycle *lifecycle, PspNetworkSupervisorEventType event,
    PspNetworkSupervisorState from, PspNetworkSupervisorState to,
    PspNetworkSupervisorState expected, uint32_t violations,
    const char *checkpoint)
{
    unsigned at = lifecycle->trace_head;
    lifecycle->trace_event[at] = event;
    lifecycle->trace_from[at] = from;
    lifecycle->trace_to[at] = to;
    lifecycle->trace_expected[at] = expected;
    lifecycle->trace_violations[at] = violations;
    lifecycle->trace_checkpoint[at] = checkpoint;
    lifecycle->trace_head = (at + 1u) % 16u;
    if (lifecycle->trace_count < 16u) lifecycle->trace_count++;
}
#endif

static void psp_network_lifecycle_dispatch(
    PspNetworkLifecycle *lifecycle, const PspNetwork *network,
    PspNetworkSupervisorEvent event,
    PspNetworkSupervisorState expected, const char *checkpoint)
{
    if (lifecycle == NULL) return;
    PspNetworkSnapshot snapshot = psp_network_lifecycle_snapshot(network);
    PspNetworkSupervisorState from = lifecycle->machine.state;
    PspNetworkSupervisorDecision decision =
        psp_network_supervisor_transition(
            &lifecycle->machine, &snapshot, &event);
    lifecycle->machine = decision.next;
    if ((decision.commands & PSP_NETWORK_COMMAND_STOP_ADMISSION) != 0)
        fetch_background_transport_set_admission(false);
    if ((decision.commands & PSP_NETWORK_COMMAND_OPEN_ADMISSION) != 0)
        fetch_background_transport_set_admission(true);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (lifecycle->events != UINT32_MAX) lifecycle->events++;
    uint32_t violations =
        psp_network_supervisor_violations(&lifecycle->machine);
    if (violations != 0 && lifecycle->violations != UINT32_MAX)
        lifecycle->violations++;
    if (expected < PSP_NETWORK_SUPERVISOR_STATE_COUNT
        && lifecycle->machine.state != expected
        && lifecycle->mismatches != UINT32_MAX)
        lifecycle->mismatches++;
    if ((decision.commands & PSP_NETWORK_COMMAND_RECORD_LEASE_WEDGE) != 0
        && lifecycle->lease_wedges != UINT32_MAX)
        lifecycle->lease_wedges++;
    psp_network_lifecycle_trace(
        lifecycle, event.type, from, lifecycle->machine.state,
        expected, violations, checkpoint);
#else
    (void) from;
    (void) expected;
    (void) checkpoint;
#endif
}

static void psp_network_lifecycle_finish_legacy_shutdown(
    PspNetworkLifecycle *lifecycle, const PspNetwork *network,
    const char *checkpoint)
{
    if (lifecycle == NULL
        || lifecycle->machine.state != PSP_NETWORK_SUPERVISOR_STOPPING)
        return;
    if (lifecycle->machine.stopping_phase == PSP_NETWORK_STOP_ADMISSION) {
        psp_network_lifecycle_dispatch(
            lifecycle, network,
            (PspNetworkSupervisorEvent) {
                .type = PSP_NETWORK_EVENT_ADMISSION_STOPPED
            }, PSP_NETWORK_SUPERVISOR_STOPPING, checkpoint);
    }
    if (lifecycle->machine.state == PSP_NETWORK_SUPERVISOR_STOPPING
        && lifecycle->machine.stopping_phase
            == PSP_NETWORK_STOP_LEAVE_APCTL) {
        psp_network_lifecycle_dispatch(
            lifecycle, network,
            (PspNetworkSupervisorEvent) {
                .type = PSP_NETWORK_EVENT_APCTL_DISCONNECTED
            }, PSP_NETWORK_SUPERVISOR_STOPPING, checkpoint);
    }
    if (lifecycle->machine.state == PSP_NETWORK_SUPERVISOR_STOPPING
        && lifecycle->machine.stopping_phase
            == PSP_NETWORK_STOP_UNWIND_RUNGS) {
        PspNetworkSupervisorState expected =
            lifecycle->machine.stopping_target
                    == PSP_NETWORK_STOP_TARGET_OFF
                ? PSP_NETWORK_SUPERVISOR_OFF
                : lifecycle->machine.stopping_target
                        == PSP_NETWORK_STOP_TARGET_OFFLINE
                    ? PSP_NETWORK_SUPERVISOR_OFFLINE
                    : lifecycle->machine.stopping_target
                            == PSP_NETWORK_STOP_TARGET_SUSPENDED_OFF
                        ? PSP_NETWORK_SUPERVISOR_SUSPENDED_OFF
                        : lifecycle->machine.stopping_target
                                == PSP_NETWORK_STOP_TARGET_RESTART
                            ? PSP_NETWORK_SUPERVISOR_STARTING
                            : PSP_NETWORK_SUPERVISOR_OFF;
        psp_network_lifecycle_dispatch(
            lifecycle, network,
            (PspNetworkSupervisorEvent) {
                .type = PSP_NETWORK_EVENT_UNWOUND
            }, expected, checkpoint);
    }
}

void psp_network_lifecycle_init(PspNetworkLifecycle *lifecycle)
{
    if (lifecycle == NULL) return;
    memset(lifecycle, 0, sizeof(*lifecycle));
    lifecycle->machine = psp_network_supervisor_initial();
    lifecycle->requests = psp_network_request_table_initial();
}

void psp_network_lifecycle_bind(PspNetworkLifecycle *lifecycle)
{
    psp_bound_network_lifecycle = lifecycle;
}

void psp_network_lifecycle_request(
    PspNetworkLifecycle *lifecycle, PspNetworkRequester requester,
    bool active, int profile_index, const PspNetwork *network,
    PspNetworkSupervisorState expected, const char *checkpoint)
{
    if (lifecycle == NULL || requester >= PSP_NETWORK_REQUEST_COUNT) return;
    uint64_t generation = ++lifecycle->next_generation;
    lifecycle->request_generation[requester] = generation;
    if (active)
        (void) psp_network_request_set(
            &lifecycle->requests, requester, generation, profile_index);
    else {
        PspNetworkRequest *request = &lifecycle->requests.requests[requester];
        request->generation = generation;
        request->active = false;
        request->profile_index = profile_index;
    }
    psp_network_lifecycle_dispatch(
        lifecycle, network,
        (PspNetworkSupervisorEvent) {
            .type = PSP_NETWORK_EVENT_TARGET_CHANGED,
            .target = psp_network_request_target(&lifecycle->requests)
        }, expected, checkpoint);
}

void psp_network_lifecycle_ladder_terminal(
    PspNetworkLifecycle *lifecycle, const PspNetwork *network,
    const char *checkpoint)
{
    if (lifecycle == NULL || network == NULL) return;
    bool ready = network->status == PSP_NETWORK_READY;
    psp_network_lifecycle_dispatch(
        lifecycle, network,
        (PspNetworkSupervisorEvent) {
            .type = ready ? PSP_NETWORK_EVENT_LADDER_READY
                          : PSP_NETWORK_EVENT_LADDER_FAILED,
            .failure = network->status == PSP_NETWORK_CANCELLED
                ? PSP_NETWORK_FAILURE_CANCELLED
                : PSP_NETWORK_FAILURE_START
        }, ready ? PSP_NETWORK_SUPERVISOR_READY
                 : PSP_NETWORK_SUPERVISOR_STOPPING,
        checkpoint);
}

void psp_network_lifecycle_suspend(
    PspNetworkLifecycle *lifecycle, const PspNetwork *network,
    bool retain_ready_stack, const char *checkpoint)
{
    psp_network_lifecycle_request(
        lifecycle, PSP_NETWORK_REQUEST_SUSPEND_INHIBIT, true, 0,
        network, PSP_NETWORK_SUPERVISOR_STOPPING, checkpoint);
    if (lifecycle != NULL && retain_ready_stack
        && lifecycle->machine.state == PSP_NETWORK_SUPERVISOR_STOPPING) {
        psp_network_lifecycle_dispatch(
            lifecycle, network,
            (PspNetworkSupervisorEvent) {
                .type = PSP_NETWORK_EVENT_ADMISSION_STOPPED
            }, PSP_NETWORK_SUPERVISOR_STATE_COUNT, checkpoint);
        if (lifecycle->machine.state == PSP_NETWORK_SUPERVISOR_STOPPING
            && lifecycle->machine.stopping_phase
                == PSP_NETWORK_STOP_DRAIN_LEASES) {
            /* Power callbacks have a bounded deadline. Stop admission and
               request cancellation, but never wait through that deadline or
               terminate sceNet beneath a curl/resolver call. A surviving
               operation becomes an explicit retained-stack lease wedge and
               is reaped after resume. */
            fetch_preconnect_cancel("suspend");
            fetch_background_transport_request_quiesce();
            bool drained = fetch_background_transport_is_quiesced();
            psp_network_lifecycle_dispatch(
                lifecycle, network,
                (PspNetworkSupervisorEvent) {
                    .type = drained
                        ? PSP_NETWORK_EVENT_LEASES_RELEASED
                        : PSP_NETWORK_EVENT_DRAIN_TIMEOUT
                }, PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED,
                drained ? "suspend-drained" : "suspend-retained");
        }
    }
}

void psp_network_lifecycle_resume_result(
    PspNetworkLifecycle *lifecycle, const PspNetwork *network,
    bool retained_ready, bool healthy, const char *checkpoint)
{
    if (lifecycle == NULL) return;
    if (retained_ready
        && lifecycle->machine.state
            == PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED
        && lifecycle->machine.stack
            == PSP_NETWORK_STACK_RETAINED_WEDGED
        && fetch_background_transport_is_quiesced()) {
        psp_network_lifecycle_dispatch(
            lifecycle, network,
            (PspNetworkSupervisorEvent) {
                .type = PSP_NETWORK_EVENT_LEASES_RELEASED
            }, PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED,
            "resume-lease-clear");
    }
    bool retained_wedged = retained_ready
        && lifecycle->machine.state
            == PSP_NETWORK_SUPERVISOR_SUSPENDED_RETAINED
        && lifecycle->machine.stack
            == PSP_NETWORK_STACK_RETAINED_WEDGED;
    psp_network_lifecycle_request(
        lifecycle, PSP_NETWORK_REQUEST_SUSPEND_INHIBIT, false, 0,
        network,
        retained_wedged ? PSP_NETWORK_SUPERVISOR_STATE_COUNT
            : retained_ready ? PSP_NETWORK_SUPERVISOR_REJOINING
                             : PSP_NETWORK_SUPERVISOR_STARTING,
        checkpoint);
    if (retained_wedged) return;
    if (retained_ready) {
        psp_network_lifecycle_dispatch(
            lifecycle, network,
            (PspNetworkSupervisorEvent) {
                .type = PSP_NETWORK_EVENT_RESUME
            }, PSP_NETWORK_SUPERVISOR_REJOINING, checkpoint);
        psp_network_lifecycle_dispatch(
            lifecycle, network,
            (PspNetworkSupervisorEvent) {
                .type = healthy ? PSP_NETWORK_EVENT_PROBE_HEALTHY
                                : PSP_NETWORK_EVENT_PROBE_FAILED
            }, healthy ? PSP_NETWORK_SUPERVISOR_READY
                       : PSP_NETWORK_SUPERVISOR_REJOINING,
            checkpoint);
    }
}

void psp_network_lifecycle_report(PspNetworkLifecycle *lifecycle)
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (lifecycle == NULL || lifecycle->events == 0) return;
    char trace[640] = "none";
    if ((lifecycle->mismatches != 0 || lifecycle->violations != 0)
        && lifecycle->trace_count != 0) {
        size_t used = 0;
        trace[0] = '\0';
        unsigned start = (lifecycle->trace_head + 16u
                          - lifecycle->trace_count) % 16u;
        for (unsigned i = 0; i < lifecycle->trace_count; i++) {
            unsigned at = (start + i) % 16u;
            int written = snprintf(
                trace + used, sizeof(trace) - used,
                "%s%u:%u>%u/%u:v%lx@%.8s",
                i == 0 ? "" : ",",
                (unsigned) lifecycle->trace_event[at],
                (unsigned) lifecycle->trace_from[at],
                (unsigned) lifecycle->trace_to[at],
                (unsigned) lifecycle->trace_expected[at],
                (unsigned long) lifecycle->trace_violations[at],
                lifecycle->trace_checkpoint[at] == NULL
                    ? "unknown" : lifecycle->trace_checkpoint[at]);
            if (written < 0 || (size_t) written >= sizeof(trace) - used) {
                trace[sizeof(trace) - 1u] = '\0';
                break;
            }
            used += (size_t) written;
        }
    }
    printf(
        "tilefinch-network-state: events=%lu mismatches=%lu "
        "violations=%lu lease-wedges=%lu health=%lu regressions=%lu "
        "state=%s failure=%u trace=%s\n",
        (unsigned long) lifecycle->events,
        (unsigned long) lifecycle->mismatches,
        (unsigned long) lifecycle->violations,
        (unsigned long) lifecycle->lease_wedges,
        (unsigned long) lifecycle->health_probes,
        (unsigned long) lifecycle->regressions,
        psp_network_supervisor_state_name(lifecycle->machine.state),
        (unsigned) lifecycle->machine.failure, trace);
#else
    (void) lifecycle;
#endif
}

bool psp_network_lifecycle_started(const PspNetworkLifecycle *lifecycle)
{
    if (lifecycle == NULL) return false;
    return lifecycle->machine.state != PSP_NETWORK_SUPERVISOR_OFF
        && lifecycle->machine.state != PSP_NETWORK_SUPERVISOR_OFFLINE
        && lifecycle->machine.state != PSP_NETWORK_SUPERVISOR_SUSPENDED_OFF;
}

bool psp_network_lifecycle_ready(const PspNetworkLifecycle *lifecycle)
{
    return lifecycle != NULL
        && lifecycle->machine.state == PSP_NETWORK_SUPERVISOR_READY;
}

bool psp_network_lifecycle_warming(const PspNetworkLifecycle *lifecycle)
{
    return lifecycle != NULL
        && lifecycle->machine.state == PSP_NETWORK_SUPERVISOR_STARTING
        && lifecycle->machine.target.cause
            == PSP_NETWORK_TARGET_CAUSE_BOOT;
}

void psp_network_lifecycle_pump(
    PspNetworkLifecycle *lifecycle, PspNetwork *network,
    uint64_t now_us)
{
    if (lifecycle == NULL || network == NULL) return;
    if (lifecycle->machine.state == PSP_NETWORK_SUPERVISOR_STARTING) {
        if (!psp_network_status_active(network->status)) {
            if (!psp_network_begin(
                    network, lifecycle->machine.target.profile_index)) {
                psp_network_lifecycle_dispatch(
                    lifecycle, network,
                    (PspNetworkSupervisorEvent) {
                        .type = PSP_NETWORK_EVENT_LADDER_FAILED,
                        .failure = PSP_NETWORK_FAILURE_START
                    }, PSP_NETWORK_SUPERVISOR_STOPPING,
                    "start-refused");
                return;
            }
        }
        (void) psp_network_pump(
            network, PSP_NETWORK_CONNECT_TIMEOUT_US);
        if (!psp_network_status_active(network->status))
            psp_network_lifecycle_ladder_terminal(
                lifecycle, network, "ladder-pump");
        return;
    }

    if (lifecycle->machine.state == PSP_NETWORK_SUPERVISOR_READY) {
        bool regression_hint =
            fetch_background_transport_take_network_regression_hint();
        if (lifecycle->next_health_probe_us == 0) {
            lifecycle->next_health_probe_us =
                now_us + UINT64_C(5000000);
            if (!regression_hint) return;
        }
        if (!regression_hint
            && now_us < lifecycle->next_health_probe_us) return;
        lifecycle->next_health_probe_us = now_us + UINT64_C(5000000);
        if (lifecycle->health_probes != UINT32_MAX)
            lifecycle->health_probes++;
        int native_result = 0;
        if (psp_network_link_ready(network, &native_result)) return;
        if (lifecycle->regressions != UINT32_MAX)
            lifecycle->regressions++;
        psp_network_lifecycle_dispatch(
            lifecycle, network,
            (PspNetworkSupervisorEvent) {
                .type = PSP_NETWORK_EVENT_REGRESSION_HINT
            }, PSP_NETWORK_SUPERVISOR_REJOINING, "health-regressed");
        /* Preserve the one-service-unit runtime contract: the corroborating
           resume/interface probe starts on the next browser frame. */
        return;
    }

    if (lifecycle->machine.state == PSP_NETWORK_SUPERVISOR_REJOINING
        && lifecycle->machine.rejoin_phase
            == PSP_NETWORK_REJOIN_PROBING) {
        int native_result = 0;
        bool healthy = psp_network_resume_ready(
            network, &native_result);
        psp_network_lifecycle_dispatch(
            lifecycle, network,
            (PspNetworkSupervisorEvent) {
                .type = healthy ? PSP_NETWORK_EVENT_PROBE_HEALTHY
                                : PSP_NETWORK_EVENT_PROBE_FAILED
            }, healthy ? PSP_NETWORK_SUPERVISOR_READY
                       : PSP_NETWORK_SUPERVISOR_REJOINING,
            healthy ? "probe-healthy" : "probe-failed");
        if (!healthy) {
            psp_network_rejoin_begin(network, &lifecycle->rejoin);
            lifecycle->rejoin_active = true;
        }
        return;
    }

    if (lifecycle->machine.state == PSP_NETWORK_SUPERVISOR_REJOINING
        && lifecycle->machine.rejoin_phase == PSP_NETWORK_REJOIN_APCTL
        && !lifecycle->rejoin_active) {
        psp_network_rejoin_begin(network, &lifecycle->rejoin);
        lifecycle->rejoin_active = true;
        return;
    }

    if (lifecycle->machine.state == PSP_NETWORK_SUPERVISOR_REJOINING
        && lifecycle->machine.rejoin_phase == PSP_NETWORK_REJOIN_APCTL
        && lifecycle->rejoin_active) {
        bool terminal = psp_network_rejoin_pump(
            network, &lifecycle->rejoin,
            PSP_NETWORK_CONNECT_TIMEOUT_US);
        if (!terminal) return;
        bool joined = lifecycle->rejoin.phase
            == PSP_NETWORK_REJOIN_COMPLETE;
        lifecycle->rejoin_active = false;
        psp_network_lifecycle_dispatch(
            lifecycle, network,
            (PspNetworkSupervisorEvent) {
                .type = joined ? PSP_NETWORK_EVENT_REJOINED
                               : PSP_NETWORK_EVENT_REJOIN_FAILED,
                .failure = joined ? PSP_NETWORK_FAILURE_NONE
                                  : PSP_NETWORK_FAILURE_REJOIN
            }, joined ? PSP_NETWORK_SUPERVISOR_READY
                      : PSP_NETWORK_SUPERVISOR_STOPPING,
            joined ? "rejoined" : "rejoin-failed");
        return;
    }

    if (lifecycle->machine.state == PSP_NETWORK_SUPERVISOR_STOPPING
        && (lifecycle->machine.stopping_target
                == PSP_NETWORK_STOP_TARGET_RESTART
            || lifecycle->machine.stopping_target
                == PSP_NETWORK_STOP_TARGET_OFFLINE)) {
        (void) psp_network_lifecycle_pump_stopping(
            lifecycle, network, now_us);
    }
}

bool psp_network_status_active(PspNetworkStatus status)
{
    return status >= PSP_NETWORK_CHECKING_PROFILE
        && status <= PSP_NETWORK_WAITING_FOR_IP;
}

static bool psp_network_lifecycle_needs_demand_recovery(
    const PspNetworkLifecycle *lifecycle)
{
    if (lifecycle == NULL) return false;
    if (lifecycle->machine.state == PSP_NETWORK_SUPERVISOR_REJOINING)
        return true;
    return lifecycle->machine.state == PSP_NETWORK_SUPERVISOR_STOPPING
        && lifecycle->machine.stopping_target
            == PSP_NETWORK_STOP_TARGET_RESTART;
}

static void psp_network_lifecycle_drive_demand_recovery(
    PspNetworkLifecycle *lifecycle, PspNetwork *network,
    const uint16_t *frame, PspUiState *ui)
{
    if (!psp_network_lifecycle_needs_demand_recovery(lifecycle)) return;
    psp_ui_set_loading(ui, true, -1);
    psp_ui_show_status(ui, "RECONNECTING NETWORK  O CANCEL", 600);
    psp_navigation_cooperate_begin(ui, frame, NULL);
    uint64_t last_present_us = 0;
    while (psp_network_lifecycle_needs_demand_recovery(lifecycle)) {
        psp_log_heartbeat();
        sceDisplayWaitVblankStart();
        SceCtrlData pad = {0};
        if (psp_navigation_cancel_requested()
            || (sceCtrlPeekBufferPositive(&pad, 1) > 0
                && (pad.Buttons & PSP_CTRL_CIRCLE) != 0)) {
            psp_ui_show_status(ui, "NETWORK CONNECTION STOPPED", 180);
            break;
        }

        uint64_t slice_started_us = sceKernelGetSystemTimeWide();
        unsigned units = psp_network_supervisor_pump_units(
            PSP_NETWORK_PUMP_DEMAND);
        for (unsigned unit = 0; unit < units; unit++) {
            PspNetworkSupervisorState before_state =
                lifecycle->machine.state;
            PspNetworkRejoinPhase before_rejoin =
                lifecycle->machine.rejoin_phase;
            PspNetworkStoppingPhase before_stopping =
                lifecycle->machine.stopping_phase;
            PspNetworkShutdownPhase before_shutdown =
                lifecycle->shutdown.phase;
            PspNetworkRejoinServicePhase before_service =
                lifecycle->rejoin.phase;
            psp_network_lifecycle_pump(
                lifecycle, network, sceKernelGetSystemTimeWide());
            if (!psp_network_lifecycle_needs_demand_recovery(lifecycle)
                || (lifecycle->machine.state == before_state
                    && lifecycle->machine.rejoin_phase == before_rejoin
                    && lifecycle->machine.stopping_phase == before_stopping
                    && lifecycle->shutdown.phase == before_shutdown
                    && lifecycle->rejoin.phase == before_service)
                || sceKernelGetSystemTimeWide() - slice_started_us
                       >= UINT64_C(8000))
                break;
        }

        uint64_t now_us = sceKernelGetSystemTimeWide();
        if (last_present_us == 0
            || now_us - last_present_us >= PSP_NETWORK_PRESENT_INTERVAL_US) {
            psp_ui_set_loading(ui, true, -1);
            if (frame != NULL && !psp_navigation_cooperate_supervised()) {
                PspUiInput idle = {.analog_x = 128, .analog_y = 128};
                (void) psp_ui_update(ui, &idle);
                psp_present(frame, ui);
            }
            last_present_us = now_us;
        }
    }
    psp_navigation_cooperate_end("network-recovery");
}

void psp_report_network_result(PspNetwork *network)
{
    if (network == NULL) return;
    printf("tilefinch-network: status=%s failure-phase=%s profile=%d/%d "
           "fallback=%d "
           "apctl=%d native=0x%08x pumps=%zu elapsed=%llums "
           "max-pump=%lluus/%s\n",
           psp_network_status_name(network->status),
           psp_network_status_name(network->failure_phase),
           network->requested_profile_index, network->profile_index,
           network->profile_fallback_used ? 1 : 0, network->apctl_state,
           (unsigned) network->native_result, network->pump_calls,
           (unsigned long long) (network->elapsed_us / 1000u),
           (unsigned long long) network->maximum_pump_us,
           psp_network_status_name(network->maximum_pump_phase));
    printf("tilefinch-network-profile: queries=0x%08x failed=0x%08x "
           "adopted=0x%08x security=%u static-ip=%d manual-dns=%d proxy=%d "
           "wlan-switch=%d wlan-power=%d\n",
           (unsigned) network->profile_query_success_mask,
           (unsigned) network->profile_query_failure_mask,
           (unsigned) network->initialization_adopted_mask,
           network->profile_security_type,
           network->profile_static_ip ? 1 : 0,
           network->profile_manual_dns ? 1 : 0,
           network->profile_uses_proxy ? 1 : 0,
           network->wlan_switch_state, network->wlan_power_state);
    printf("tilefinch-network-memory: free=%zu minimum=%zu ready=%zu "
           "largest=%zu minimum-largest=%zu ready-largest=%zu\n",
           network->free_memory_start, network->free_memory_minimum,
           network->free_memory_ready, network->maximum_free_block_start,
           network->maximum_free_block_minimum,
           network->maximum_free_block_ready);
    for (int phase = PSP_NETWORK_CHECKING_PROFILE;
         phase <= PSP_NETWORK_WAITING_FOR_IP; phase++) {
        if (network->phase_pump_calls[phase] == 0) continue;
        printf("tilefinch-network-phase: phase=%s calls=%zu total=%lluus\n",
               psp_network_status_name((PspNetworkStatus) phase),
               network->phase_pump_calls[phase],
               (unsigned long long) network->phase_pump_us[phase]);
    }
    if (network->status == PSP_NETWORK_READY) {
        PspNetworkInterfaceReport interface_report;
        bool interface_ready = psp_network_interface_report(
            network, &interface_report);
        printf("tilefinch-network-interface: ready=%d queries=0x%08x "
               "failed=0x%08x first-error=0x%08x security=%u "
               "strength=%u channel=%u power-save=%d proxy=%d "
               "ip=%d subnet=%d gateway=%d dns=%d/%d redacted=yes\n",
               interface_ready ? 1 : 0,
               (unsigned) interface_report.query_success_mask,
               (unsigned) interface_report.query_failure_mask,
               (unsigned) interface_report.first_error,
               interface_report.security_type,
               interface_report.signal_strength,
               interface_report.channel,
               interface_report.power_save ? 1 : 0,
               interface_report.uses_proxy ? 1 : 0,
               interface_report.has_ip ? 1 : 0,
               interface_report.has_subnet ? 1 : 0,
               interface_report.has_gateway ? 1 : 0,
               interface_report.has_primary_dns ? 1 : 0,
               interface_report.has_secondary_dns ? 1 : 0);
    }
}

bool psp_connect_network(PspNetwork *network, int profile_index,
                                const uint16_t *frame, PspUiState *ui)
{
    if (network == NULL) {
        printf("tilefinch-network: begin refused reason=no-state "
               "profile=%d\n", profile_index);
        psp_log_checkpoint("network-begin-refused");
        return false;
    }
    /* A navigation begins only after text/voice entry has returned. Retire
       the voice memory inhibit before publishing the stronger navigation
       demand; otherwise its deliberate precedence would keep the target off. */
    psp_network_lifecycle_request(
        psp_bound_network_lifecycle, PSP_NETWORK_REQUEST_VOICE_INHIBIT,
        false, 0, network, PSP_NETWORK_SUPERVISOR_STATE_COUNT,
        "voice-done");
    psp_network_lifecycle_request(
        psp_bound_network_lifecycle, PSP_NETWORK_REQUEST_NAVIGATION,
        true, profile_index, network,
        psp_network_status_active(network->status)
            ? PSP_NETWORK_SUPERVISOR_STARTING
            : network->status == PSP_NETWORK_READY
                ? PSP_NETWORK_SUPERVISOR_READY
                : PSP_NETWORK_SUPERVISOR_STARTING,
        "connect");
    psp_network_lifecycle_drive_demand_recovery(
        psp_bound_network_lifecycle, network, frame, ui);
    if (psp_network_lifecycle_ready(psp_bound_network_lifecycle)
        && network->status == PSP_NETWORK_READY) return true;
    if (psp_network_lifecycle_needs_demand_recovery(
            psp_bound_network_lifecycle)
        || (psp_bound_network_lifecycle != NULL
            && psp_bound_network_lifecycle->machine.state
                == PSP_NETWORK_SUPERVISOR_OFFLINE)) {
        psp_ui_set_loading(ui, false, 0);
        if (psp_bound_network_lifecycle != NULL
            && psp_bound_network_lifecycle->machine.state
                == PSP_NETWORK_SUPERVISOR_OFFLINE)
            psp_ui_show_status(ui, "NETWORK UNAVAILABLE", 240);
        if (frame != NULL) psp_present(frame, ui);
        return false;
    }
    bool adopted = psp_network_status_active(network->status);
    if (network->status == PSP_NETWORK_READY) {
        if (psp_bound_network_lifecycle != NULL
            && psp_bound_network_lifecycle->machine.state
                == PSP_NETWORK_SUPERVISOR_REJOINING) {
            psp_network_lifecycle_dispatch(
                psp_bound_network_lifecycle, network,
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_PROBE_HEALTHY
                }, PSP_NETWORK_SUPERVISOR_READY, "connect-ready");
        } else if (!psp_network_lifecycle_ready(
                       psp_bound_network_lifecycle)) {
            psp_network_lifecycle_ladder_terminal(
                psp_bound_network_lifecycle, network, "connect-ready");
        }
        return psp_network_lifecycle_ready(psp_bound_network_lifecycle);
    }
    if (!adopted && !psp_network_begin(network, profile_index)) {
        printf("tilefinch-network: begin refused reason=invalid-profile "
               "profile=%d status=%s\n", profile_index,
               psp_network_status_name(network->status));
        psp_log_checkpoint("network-begin-refused");
        return false;
    }
    if (adopted) {
        printf("tilefinch-network-warmup: status=adopted phase=%s "
               "elapsed=%llums\n",
               psp_network_status_name(network->status),
               (unsigned long long) (network->elapsed_us / 1000u));
    }
    psp_ui_set_loading(ui, true, -1);
    psp_ui_show_status(ui, "CONNECTING NETWORK  O CANCEL", 600);
    psp_navigation_cooperate_begin(ui, frame, NULL);
    PspNetworkStatus previous = PSP_NETWORK_IDLE;
    int previous_apctl = -2;
    unsigned frames = 0;
    uint64_t last_present_us = 0;
    for (;;) {
        psp_log_heartbeat();
        sceDisplayWaitVblankStart();
        SceCtrlData pad = {0};
        if (psp_navigation_cancel_requested()
            || (sceCtrlPeekBufferPositive(&pad, 1) > 0
                && (pad.Buttons & PSP_CTRL_CIRCLE) != 0)) {
            psp_network_mark_cancelled(network);
        } else {
            /* A foreground navigation must not pay one 30 Hz frame for each
               cheap setup rung. Advance changed, non-association phases to
               quiescence within a small frame budget. Background warmup
               intentionally keeps its one-unit cadence in the main loop. */
            uint64_t slice_started_us = sceKernelGetSystemTimeWide();
            unsigned units = psp_network_supervisor_pump_units(
                PSP_NETWORK_PUMP_DEMAND);
            for (unsigned unit = 0; unit < units; unit++) {
                PspNetworkStatus before = network->status;
                (void) psp_network_pump(
                    network, PSP_NETWORK_CONNECT_TIMEOUT_US);
                if (!psp_network_status_active(network->status)
                    || network->status == PSP_NETWORK_WAITING_FOR_IP
                    || network->status == before
                    || sceKernelGetSystemTimeWide() - slice_started_us
                           >= UINT64_C(8000))
                    break;
            }
        }
        frames++;
        bool stage_changed = network->status != previous
            || network->apctl_state != previous_apctl;
        bool periodic = (frames & 15u) == 0;
        bool terminal = network->status == PSP_NETWORK_READY
            || network->status == PSP_NETWORK_FAILED
            || network->status == PSP_NETWORK_CANCELLED;
        if (stage_changed || periodic) {
            uint64_t now_us = sceKernelGetSystemTimeWide();
            bool presentation_due = !terminal
                && (last_present_us == 0
                    || now_us - last_present_us
                           >= PSP_NETWORK_PRESENT_INTERVAL_US);
            char status[64];
            snprintf(status, sizeof(status), "NETWORK %s - CIRCLE CANCELS",
                     psp_network_status_name(network->status));
            if (presentation_due) {
                psp_ui_show_status(ui, status, 120);
                psp_ui_set_loading(ui, true, -1);
                if (frame != NULL
                    && !psp_navigation_cooperate_supervised()) {
                    PspUiInput idle = {
                        .analog_x = 128,
                        .analog_y = 128
                    };
                    (void) psp_ui_update(ui, &idle);
                    psp_present(frame, ui);
                }
                last_present_us = now_us;
            }
            if (stage_changed) {
                printf("tilefinch-network-stage: status=%s apctl=%d "
                       "elapsed=%llums\n",
                       psp_network_status_name(network->status),
                       network->apctl_state,
                       (unsigned long long) (network->elapsed_us / 1000u));
                previous_apctl = network->apctl_state;
            }
            previous = network->status;
        }
        if (terminal) break;
    }
    psp_report_network_result(network);
    psp_network_lifecycle_ladder_terminal(
        psp_bound_network_lifecycle, network, "connect-end");
    bool ready = network->status == PSP_NETWORK_READY;
    if (!ready) {
        bool cancelled = network->status == PSP_NETWORK_CANCELLED;
        bool saved = !cancelled
            && psp_write_network_failure_report(network);
        psp_log_checkpoint(
            cancelled
                ? "network-connect-cancelled"
                : "network-connect-failed");
        psp_ui_show_status(
            ui,
            cancelled ? "NETWORK CONNECTION STOPPED"
                      : (saved ? "NETWORK UNAVAILABLE - DETAILS SAVED"
                               : "NETWORK UNAVAILABLE"),
            240);
    }
    psp_navigation_cooperate_end("network");
    psp_ui_set_loading(ui, ready, ready ? -1 : 0);
    if (ready) psp_ui_show_status(ui, "OPENING PAGE", 30);
    if (frame != NULL) psp_present(frame, ui);
    return ready;
}

static void psp_network_shutdown_report_log(
    const PspNetworkShutdownReport *report)
{
    if (report == NULL) return;
    printf("tilefinch-network-shutdown: attempted=0x%08x failed=0x%08x "
           "first-error=0x%08x apctl=%d disconnect=%zu/%lluus "
           "elapsed=%lluus free=%zu/%zu largest=%zu/%zu\n",
           (unsigned) report->attempted_mask,
           (unsigned) report->failure_mask,
           (unsigned) report->first_error,
           report->final_apctl_state, report->disconnect_polls,
           (unsigned long long) report->disconnect_wait_us,
           (unsigned long long) report->elapsed_us,
           report->free_memory_before, report->free_memory_after,
           report->maximum_free_block_before,
           report->maximum_free_block_after);
}

static bool psp_network_lifecycle_pump_stopping(
    PspNetworkLifecycle *lifecycle, PspNetwork *network,
    uint64_t now_us)
{
    if (lifecycle == NULL || network == NULL
        || lifecycle->machine.state != PSP_NETWORK_SUPERVISOR_STOPPING)
        return true;

    if (lifecycle->machine.stopping_phase
        == PSP_NETWORK_STOP_ADMISSION) {
        psp_network_lifecycle_dispatch(
            lifecycle, network,
            (PspNetworkSupervisorEvent) {
                .type = PSP_NETWORK_EVENT_ADMISSION_STOPPED
            }, PSP_NETWORK_SUPERVISOR_STATE_COUNT, "stop-admit");
        if (lifecycle->machine.state != PSP_NETWORK_SUPERVISOR_STOPPING)
            return true;
    }

    if (lifecycle->machine.stopping_phase
            == PSP_NETWORK_STOP_DRAIN_LEASES
        || lifecycle->machine.stopping_phase
            == PSP_NETWORK_STOP_LEASE_WEDGED) {
        if (!lifecycle->quiesce_requested) {
            fetch_preconnect_cancel("network-shutdown");
            fetch_background_transport_request_quiesce();
            lifecycle->quiesce_requested = true;
            lifecycle->drain_deadline_us =
                now_us + UINT64_C(4000000);
        }
        if (fetch_background_transport_is_quiesced()) {
            psp_network_lifecycle_dispatch(
                lifecycle, network,
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_LEASES_RELEASED
                }, PSP_NETWORK_SUPERVISOR_STATE_COUNT, "leases-clear");
            lifecycle->quiesce_requested = false;
        } else if (lifecycle->machine.stopping_phase
                       == PSP_NETWORK_STOP_DRAIN_LEASES
                   && now_us >= lifecycle->drain_deadline_us) {
            psp_network_lifecycle_dispatch(
                lifecycle, network,
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_DRAIN_TIMEOUT
                }, PSP_NETWORK_SUPERVISOR_STOPPING, "lease-wedge");
            printf("tilefinch-network-shutdown: background-transport=busy "
                   "action=retain-stack leases=%zu\n",
                   fetch_background_transport_active_operations());
        }
        if (lifecycle->machine.stopping_phase
                == PSP_NETWORK_STOP_DRAIN_LEASES
            || lifecycle->machine.stopping_phase
                == PSP_NETWORK_STOP_LEASE_WEDGED)
            return false;
    }

    if (lifecycle->machine.state != PSP_NETWORK_SUPERVISOR_STOPPING)
        return true;
    if (lifecycle->machine.stopping_phase != PSP_NETWORK_STOP_LEAVE_APCTL
        && lifecycle->machine.stopping_phase
            != PSP_NETWORK_STOP_UNWIND_RUNGS)
        return false;

    if (!lifecycle->shutdown_active) {
        psp_network_shutdown_begin(network, &lifecycle->shutdown);
        lifecycle->shutdown_active = true;
        if (lifecycle->shutdown.phase != PSP_NETWORK_SHUTDOWN_LEAVING) {
            psp_network_lifecycle_dispatch(
                lifecycle, network,
                (PspNetworkSupervisorEvent) {
                    .type = PSP_NETWORK_EVENT_APCTL_DISCONNECTED
                }, PSP_NETWORK_SUPERVISOR_STOPPING, "leave-empty");
        }
    }
    PspNetworkShutdownPhase previous_phase = lifecycle->shutdown.phase;
    bool complete = psp_network_shutdown_pump(
        network, &lifecycle->shutdown);
    if (previous_phase == PSP_NETWORK_SHUTDOWN_LEAVING
        && lifecycle->shutdown.phase != PSP_NETWORK_SHUTDOWN_LEAVING) {
        psp_network_lifecycle_dispatch(
            lifecycle, network,
            (PspNetworkSupervisorEvent) {
                .type = lifecycle->shutdown.leave_timed_out
                    ? PSP_NETWORK_EVENT_LEAVE_TIMEOUT
                    : PSP_NETWORK_EVENT_APCTL_DISCONNECTED
            }, PSP_NETWORK_SUPERVISOR_STOPPING, "leave-finished");
    } else if (previous_phase >= PSP_NETWORK_SHUTDOWN_TERM_APCTL
               && previous_phase < PSP_NETWORK_SHUTDOWN_UNLOAD_COMMON
               && lifecycle->shutdown.phase != previous_phase) {
        psp_network_lifecycle_dispatch(
            lifecycle, network,
            (PspNetworkSupervisorEvent) {
                .type = PSP_NETWORK_EVENT_RUNG_UNWOUND
            }, PSP_NETWORK_SUPERVISOR_STOPPING, "rung-unwound");
    }
    if (!complete) return false;
    psp_network_shutdown_report_log(&lifecycle->shutdown.report);
    lifecycle->shutdown_active = false;
    lifecycle->quiesce_requested = false;
    psp_network_lifecycle_finish_legacy_shutdown(
        lifecycle, network, "shutdown");
    return lifecycle->machine.state != PSP_NETWORK_SUPERVISOR_STOPPING;
}

bool psp_shutdown_network_logged(PspNetwork *network)
{
    if (psp_bound_network_lifecycle != NULL
        && psp_bound_network_lifecycle->machine.state
            != PSP_NETWORK_SUPERVISOR_STOPPING) {
        psp_network_lifecycle_dispatch(
            psp_bound_network_lifecycle, network,
            (PspNetworkSupervisorEvent) {
                .type = PSP_NETWORK_EVENT_TARGET_CHANGED,
                .target = {
                    .kind = PSP_NETWORK_TARGET_OFF,
                    .cause = PSP_NETWORK_TARGET_CAUSE_NONE,
                    .profile_index = 0
                }
            }, PSP_NETWORK_SUPERVISOR_STOPPING, "legacy-off");
    }
    if (psp_bound_network_lifecycle == NULL) {
        if (!fetch_background_transport_quiesce(4000u)) return false;
        PspNetworkShutdownReport report;
        psp_network_shutdown(network, &report);
        psp_network_shutdown_report_log(&report);
        return true;
    }
    while (psp_bound_network_lifecycle->machine.state
           == PSP_NETWORK_SUPERVISOR_STOPPING) {
        bool complete = psp_network_lifecycle_pump_stopping(
            psp_bound_network_lifecycle, network,
            sceKernelGetSystemTimeWide());
        if (complete) return true;
        if (psp_bound_network_lifecycle->machine.stopping_phase
            == PSP_NETWORK_STOP_LEASE_WEDGED)
            return false;
        psp_log_heartbeat();
        bool leaving = psp_bound_network_lifecycle->shutdown_active
            && psp_bound_network_lifecycle->shutdown.phase
                == PSP_NETWORK_SHUTDOWN_LEAVING;
        (void) sceKernelDelayThread(leaving ? 5000u : 1000u);
    }
    return true;
}

bool psp_ensure_network_for_navigation(
    PspNetwork *network, PspNetworkLifecycle *lifecycle, int profile_index,
    const char *method, const char *url, bool present_destination,
    const uint16_t *frame, PspUiState *ui)
{
    if (psp_ui_native_home_url(url)) return true;
    if (psp_profile_page_kind(url) != PSP_PROFILE_PAGE_NONE) return true;
    if (!site_adapter_navigation_requires_network(method, url)) return true;
    /* Association is part of this navigation's loading experience. Show the
       accepted destination throughout it instead of the incumbent address. */
    if (present_destination) psp_ui_set_navigation_target(ui, url);
    if (network == NULL || lifecycle == NULL) return false;
    if (psp_network_lifecycle_ready(lifecycle)
        && network->status == PSP_NETWORK_READY) return true;
    if (psp_network_lifecycle_started(lifecycle)
        && !psp_network_status_active(network->status)
        && !psp_network_lifecycle_needs_demand_recovery(lifecycle)) {
        if (!psp_shutdown_network_logged(network)) return false;
    }
    bool ready = psp_connect_network(network, profile_index, frame, ui);
    if (!ready && psp_network_lifecycle_started(lifecycle)
        && !psp_network_lifecycle_needs_demand_recovery(lifecycle)) {
        (void) psp_shutdown_network_logged(network);
    }
    return ready;
}
#endif
