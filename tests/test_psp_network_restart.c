#include "tilefinch/psp_network.h"
#include <string.h>
#include <stdio.h>

#define PSP_NET_APCTL_STATE_DISCONNECTED 0
#define PSP_NET_MODULE_COMMON 1
#define PSP_NET_MODULE_INET 3
#define SCE_ERROR_NET_MODULE_NOT_LOADED UINT32_C(0x80110803)
#define PSP_NETWORK_DISCONNECT_WAIT_US UINT64_C(250000)
#define PSP_NETWORK_DISCONNECT_POLL_US 5000u
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "network restart: line %d: %s\n", __LINE__, #c); return 1; } } while (0)
static unsigned terms, unloads, disconnects;
static unsigned refuse_unloads;
static int unload_failure = -1;
static int apctl_state;
static uint64_t now;
static uint64_t psp_network_now_us(void) { return ++now; }
static int sceKernelTotalFreeMemSize(void) { return 1024; }
static int sceKernelMaxFreeMemSize(void) { return 512; }
static int sceKernelDelayThread(unsigned us) { now += us; return 0; }
static int sceNetApctlGetState(int *state) { *state = apctl_state; return 0; }
static int sceNetApctlDisconnect(void) { disconnects++; apctl_state = 0; return 0; }
static int sceNetApctlTerm(void) { terms |= 1; return 0; }
static int sceNetResolverTerm(void) { terms |= 2; return 0; }
static int sceNetInetTerm(void) { terms |= 4; return 0; }
static int sceNetTerm(void) { terms |= 8; return 0; }
static int sceUtilityUnloadNetModule(int id)
{
    unsigned bit = id == PSP_NET_MODULE_COMMON ? 1u : 2u;
    unloads |= bit;
    return (refuse_unloads & bit) != 0 ? unload_failure : 0;
}
#include "../src/psp_network_shutdown.inc"

int main(void)
{
    for (unsigned owned = 0; owned < 4; owned++) {
        PspNetwork n = {.status = PSP_NETWORK_READY,
            .common_loaded = true, .inet_loaded = true,
            .common_owned = (owned & 1) != 0, .inet_owned = (owned & 2) != 0,
            .core_initialized = true, .inet_initialized = true,
            .resolver_initialized = true, .apctl_initialized = true,
            .connect_started = true};
        PspNetworkShutdownOperation op;
        terms = unloads = disconnects = 0; apctl_state = 4;
        psp_network_shutdown_begin(&n, &op);
        op.retain_modules = true;
        unsigned pumps = 0;
        while (!psp_network_shutdown_pump(&n, &op)) CHECK(++pumps < 16);
        CHECK(terms == 15 && disconnects == 1 && unloads == 0);
        CHECK(n.common_loaded && n.inet_loaded);
        CHECK(n.common_owned == ((owned & 1) != 0));
        CHECK(n.inet_owned == ((owned & 2) != 0));
        CHECK(n.status == PSP_NETWORK_IDLE && !n.core_initialized
            && !n.inet_initialized && !n.resolver_initialized && !n.apctl_initialized);
        CHECK(op.report.failure_mask == 0 && op.report.free_memory_after == 1024);
        psp_network_reset_ladder(&n);
        CHECK(n.common_loaded && n.inet_loaded);
        CHECK(n.common_owned == ((owned & 1) != 0));
        CHECK(n.inet_owned == ((owned & 2) != 0));
        /* Cancellation/exit after a retained restart still owns both PRXs. */
        psp_network_shutdown(&n, NULL);
        CHECK(unloads == owned && !n.common_loaded && !n.inet_loaded);
        CHECK(!n.common_owned && !n.inet_owned);
    }
    /* Changing the recovery target to Off before service retirement must
       perform the ordinary complete teardown, not leave modules behind. */
    PspNetwork n = {.common_loaded = true, .inet_loaded = true,
        .common_owned = true, .inet_owned = true, .core_initialized = true};
    PspNetworkShutdownOperation op;
    terms = unloads = 0;
    psp_network_shutdown_begin(&n, &op);
    op.retain_modules = true;
    CHECK(!psp_network_shutdown_pump(&n, &op));
    op.retain_modules = false;
    unsigned pumps = 0;
    while (!psp_network_shutdown_pump(&n, &op)) CHECK(++pumps < 16);
    CHECK(unloads == 3 && !n.common_loaded && !n.inet_loaded);
    /* A refused unload still owns the PRX. Complete this bounded shutdown,
       then retry only the retained owner on a later ordinary shutdown. */
    for (unsigned refused = 1; refused < 4; refused++) {
        n = (PspNetwork) {.common_loaded = true, .inet_loaded = true,
            .common_owned = true, .inet_owned = true};
        refuse_unloads = refused;
        unloads = 0;
        PspNetworkShutdownReport report;
        psp_network_shutdown(&n, &report);
        CHECK(unloads == 3 && report.first_error == unload_failure);
        CHECK(report.failure_mask == (((refused & 1u) ? 1u << 6 : 0u)
                                    | ((refused & 2u) ? 1u << 5 : 0u)));
        CHECK(n.common_owned == ((refused & 1u) != 0)
              && n.common_loaded == n.common_owned);
        CHECK(n.inet_owned == ((refused & 2u) != 0)
              && n.inet_loaded == n.inet_owned);
        refuse_unloads = unloads = 0;
        psp_network_shutdown(&n, &report);
        CHECK(unloads == refused && report.failure_mask == 0);
        CHECK(!n.common_owned && !n.common_loaded
              && !n.inet_owned && !n.inet_loaded);
    }
    /* The SDK's explicit already-absent result is not a retained owner. */
    n = (PspNetwork) {.common_loaded = true, .inet_loaded = true,
        .common_owned = true, .inet_owned = true};
    refuse_unloads = 3;
    unload_failure = (int) SCE_ERROR_NET_MODULE_NOT_LOADED;
    PspNetworkShutdownReport absent;
    psp_network_shutdown(&n, &absent);
    CHECK(absent.attempted_mask == ((1u << 5) | (1u << 6))
          && absent.failure_mask == absent.attempted_mask
          && absent.first_error == unload_failure);
    CHECK(!n.common_owned && !n.common_loaded
          && !n.inet_owned && !n.inet_loaded);
    unloads = 0;
    psp_network_shutdown(&n, NULL);
    CHECK(unloads == 0);
    puts("network restart ownership checks passed");
    return 0;
}
