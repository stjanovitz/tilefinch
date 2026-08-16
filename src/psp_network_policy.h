#ifndef TILEFINCH_PSP_NETWORK_POLICY_H
#define TILEFINCH_PSP_NETWORK_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* These process-local network services can survive an incomplete PSP teardown.
   A subsequent initialization then reports the existing service with a
   negative status even though it is usable.  Treat only the exact firmware
   statuses as adoption; every other negative result remains a real failure. */
#define PSP_NETWORK_CORE_NOT_TERMINATED UINT32_C(0x80410101)
#define PSP_NETWORK_INET_ALREADY_INITIALIZED UINT32_C(0x80410201)
#define PSP_NETWORK_RESOLVER_NOT_TERMINATED UINT32_C(0x80410401)
#define PSP_NETWORK_APCTL_ALREADY_INITIALIZED UINT32_C(0x80410a01)

typedef enum {
    PSP_NETWORK_INIT_CORE = 0,
    PSP_NETWORK_INIT_INET,
    PSP_NETWORK_INIT_RESOLVER,
    PSP_NETWORK_INIT_APCTL
} PspNetworkInitService;

typedef int (*PspNetworkProfileCheck)(int profile, void *context);

/* Returns the first valid saved profile other than the requested slot. A
   zero result means none exists. The callback form keeps the bounded choice
   policy host-testable while the PSP implementation supplies the firmware
   query. */
static inline int psp_network_first_fallback_profile(
    int requested, int maximum, PspNetworkProfileCheck check, void *context)
{
    if (requested <= 0 || maximum <= 0 || check == NULL) return 0;
    for (int candidate = 1; candidate <= maximum; candidate++) {
        if (candidate == requested) continue;
        if (check(candidate, context) == 0) return candidate;
    }
    return 0;
}

/* Walk saved profiles cyclically in the requested direction. Including the
   starting slot only after every other slot has been considered means a
   single-profile PSP remains stable while a PSP with several configurations
   always advances to a different valid one. */
static inline int psp_network_next_saved_profile(
    int current, int maximum, int direction,
    PspNetworkProfileCheck check, void *context)
{
    if (current <= 0 || current > maximum || maximum <= 0
        || direction == 0 || check == NULL) return 0;
    int step = direction < 0 ? -1 : 1;
    int candidate = current;
    for (int visited = 0; visited < maximum; visited++) {
        candidate += step;
        if (candidate < 1) candidate = maximum;
        else if (candidate > maximum) candidate = 1;
        if (check(candidate, context) == 0) return candidate;
    }
    return 0;
}

/* APCTL states 2, 3, 5, and 6 are an association already in progress on
   6.6x firmware.  This is deliberately expressed in terms of the public
   state values rather than accepting an arbitrary non-disconnected state:
   SCANNING belongs to a different operation and must not be mistaken for a
   connection which will eventually produce an IP address. */
static inline bool psp_network_apctl_state_is_associating(int state)
{
    return state == 2 || state == 3 || state == 5 || state == 6;
}

static inline bool psp_network_init_result_usable(
    PspNetworkInitService service, int result, bool *adopted)
{
    if (adopted != NULL) *adopted = false;
    if (result >= 0) return true;
    uint32_t code = (uint32_t) result;
    bool existing =
        (service == PSP_NETWORK_INIT_CORE
            && code == PSP_NETWORK_CORE_NOT_TERMINATED)
        || (service == PSP_NETWORK_INIT_INET
            && code == PSP_NETWORK_INET_ALREADY_INITIALIZED)
        || (service == PSP_NETWORK_INIT_RESOLVER
            && code == PSP_NETWORK_RESOLVER_NOT_TERMINATED)
        || (service == PSP_NETWORK_INIT_APCTL
            && code == PSP_NETWORK_APCTL_ALREADY_INITIALIZED);
    if (existing && adopted != NULL) *adopted = true;
    return existing;
}

#endif
