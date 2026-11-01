/* A restart discards service/connection state, not the ownership of modules
   which never left memory. A later ordinary shutdown must still unload them. */
static void psp_network_reset_ladder(PspNetwork *network)
{
    bool common_loaded = network->common_loaded;
    bool inet_loaded = network->inet_loaded;
    bool common_owned = network->common_owned;
    bool inet_owned = network->inet_owned;
    memset(network, 0, sizeof(*network));
    network->common_loaded = common_loaded;
    network->inet_loaded = inet_loaded;
    network->common_owned = common_owned;
    network->inet_owned = inet_owned;
}

static void psp_network_shutdown_result(
    PspNetworkShutdownReport *report, uint32_t bit, int result)
{
    report->attempted_mask |= bit;
    if (result < 0) {
        report->failure_mask |= bit;
        if (report->first_error == 0) report->first_error = result;
    }
}

void psp_network_shutdown_begin(
    PspNetwork *network, PspNetworkShutdownOperation *operation)
{
    if (operation == NULL) return;
    memset(operation, 0, sizeof(*operation));
    if (network == NULL) {
        operation->phase = PSP_NETWORK_SHUTDOWN_COMPLETE;
        return;
    }
    operation->started_us = psp_network_now_us();
    operation->leave_started_us = operation->started_us;
    operation->report.final_apctl_state = network->apctl_state;
    operation->report.free_memory_before =
        (size_t) sceKernelTotalFreeMemSize();
    operation->report.maximum_free_block_before =
        (size_t) sceKernelMaxFreeMemSize();
    operation->phase = network->apctl_initialized
        ? PSP_NETWORK_SHUTDOWN_LEAVING
        : PSP_NETWORK_SHUTDOWN_TERM_RESOLVER;
}

static void psp_network_shutdown_finish(
    PspNetwork *network, PspNetworkShutdownOperation *operation)
{
    PspNetworkShutdownReport *report = &operation->report;
    report->elapsed_us = psp_network_now_us() - operation->started_us;
    report->free_memory_after = (size_t) sceKernelTotalFreeMemSize();
    report->maximum_free_block_after = (size_t) sceKernelMaxFreeMemSize();
    psp_network_reset_ladder(network);
    operation->phase = PSP_NETWORK_SHUTDOWN_COMPLETE;
}

static bool psp_network_shutdown_unload(
    PspNetworkShutdownReport *report, int module, uint32_t bit, bool owned)
{
    if (!owned) return true;
    int result = sceUtilityUnloadNetModule(module);
    psp_network_shutdown_result(report, bit, result);
    /* A refused unload keeps ownership for a later bounded shutdown, but
       the SDK's explicit already-absent result needs no later retry. */
    return result >= 0
        || (uint32_t) result == SCE_ERROR_NET_MODULE_NOT_LOADED;
}

bool psp_network_shutdown_pump(
    PspNetwork *network, PspNetworkShutdownOperation *operation)
{
    if (network == NULL || operation == NULL
        || operation->phase == PSP_NETWORK_SHUTDOWN_COMPLETE)
        return true;
    PspNetworkShutdownReport *report = &operation->report;
    switch (operation->phase) {
    case PSP_NETWORK_SHUTDOWN_LEAVING: {
        int state = PSP_NET_APCTL_STATE_DISCONNECTED;
        int queried = sceNetApctlGetState(&state);
        report->disconnect_polls++;
        if (queried < 0) {
            psp_network_shutdown_result(
                report, UINT32_C(1) << 7, queried);
            state = network->apctl_state;
            operation->leave_timed_out = true;
            operation->phase = PSP_NETWORK_SHUTDOWN_TERM_APCTL;
        } else {
            network->apctl_state = state;
            report->final_apctl_state = state;
            if (state == PSP_NET_APCTL_STATE_DISCONNECTED) {
                operation->phase = PSP_NETWORK_SHUTDOWN_TERM_APCTL;
            } else if (!network->disconnect_started) {
                int disconnected = sceNetApctlDisconnect();
                psp_network_shutdown_result(
                    report, UINT32_C(1) << 0, disconnected);
                if (disconnected >= 0)
                    network->disconnect_started = true;
                else {
                    operation->leave_timed_out = true;
                    operation->phase = PSP_NETWORK_SHUTDOWN_TERM_APCTL;
                }
            } else if (psp_network_now_us() - operation->leave_started_us
                       >= PSP_NETWORK_DISCONNECT_WAIT_US) {
                operation->leave_timed_out = true;
                operation->phase = PSP_NETWORK_SHUTDOWN_TERM_APCTL;
            }
        }
        report->disconnect_wait_us =
            psp_network_now_us() - operation->leave_started_us;
        return false;
    }
    case PSP_NETWORK_SHUTDOWN_TERM_APCTL:
        if (network->apctl_initialized)
            psp_network_shutdown_result(
                report, UINT32_C(1) << 1, sceNetApctlTerm());
        network->apctl_initialized = false;
        network->connect_started = false;
        network->disconnect_started = false;
        operation->phase = PSP_NETWORK_SHUTDOWN_TERM_RESOLVER;
        return false;
    case PSP_NETWORK_SHUTDOWN_TERM_RESOLVER:
        if (network->resolver_initialized)
            psp_network_shutdown_result(
                report, UINT32_C(1) << 2, sceNetResolverTerm());
        network->resolver_initialized = false;
        operation->phase = PSP_NETWORK_SHUTDOWN_TERM_INET;
        return false;
    case PSP_NETWORK_SHUTDOWN_TERM_INET:
        if (network->inet_initialized)
            psp_network_shutdown_result(
                report, UINT32_C(1) << 3, sceNetInetTerm());
        network->inet_initialized = false;
        operation->phase = PSP_NETWORK_SHUTDOWN_TERM_CORE;
        return false;
    case PSP_NETWORK_SHUTDOWN_TERM_CORE:
        if (network->core_initialized)
            psp_network_shutdown_result(
                report, UINT32_C(1) << 4, sceNetTerm());
        network->core_initialized = false;
        if (operation->retain_modules) {
            psp_network_shutdown_finish(network, operation);
            return true;
        }
        operation->phase = PSP_NETWORK_SHUTDOWN_UNLOAD_INET;
        return false;
    case PSP_NETWORK_SHUTDOWN_UNLOAD_INET:
        if (psp_network_shutdown_unload(report, PSP_NET_MODULE_INET,
                UINT32_C(1) << 5, network->inet_owned)) {
            network->inet_owned = false;
            network->inet_loaded = false;
        }
        operation->phase = PSP_NETWORK_SHUTDOWN_UNLOAD_COMMON;
        return false;
    case PSP_NETWORK_SHUTDOWN_UNLOAD_COMMON:
        if (psp_network_shutdown_unload(report, PSP_NET_MODULE_COMMON,
                UINT32_C(1) << 6, network->common_owned)) {
            network->common_owned = false;
            network->common_loaded = false;
        }
        psp_network_shutdown_finish(network, operation);
        return true;
    case PSP_NETWORK_SHUTDOWN_IDLE:
    case PSP_NETWORK_SHUTDOWN_COMPLETE:
    default:
        return true;
    }
}

void psp_network_shutdown(
    PspNetwork *network, PspNetworkShutdownReport *report)
{
    if (network == NULL) return;
    PspNetworkShutdownOperation operation;
    psp_network_shutdown_begin(network, &operation);
    while (!psp_network_shutdown_pump(network, &operation)) {
        if (operation.phase == PSP_NETWORK_SHUTDOWN_LEAVING)
            (void) sceKernelDelayThread(PSP_NETWORK_DISCONNECT_POLL_US);
    }
    if (report != NULL) *report = operation.report;
}
