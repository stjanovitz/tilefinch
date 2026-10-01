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

/*
 * A name lookup the firmware resolver has not finished by this bound is
 * stopped from outside. sceNetResolverStartNtoA takes a timeout and a retry
 * count, but on a PSP-3000 about one lookup in fifteen never returns at all
 * (a transport worker sat in one for over thirty seconds), and every request
 * queued behind that worker froze with it. Healthy lookups finished in 7-61
 * ms across every device run, and a hung one has never recovered by itself,
 * so the bound is short; gethostbyname then retries once with a fresh
 * resolver, which is how a stopped lookup has succeeded on hardware.
 *
 * Hangs can be consecutive, though: one device run lost both attempts for a
 * googlevideo host (3 s each, start=0x8041040E stopped=1) and the video
 * never opened. The firmware resolver is therefore no longer the first
 * resort. gethostbyname asks the access point's DNS servers itself over UDP
 * (src/psp_dns_stub.h, at most 2.4 s) and reaches this guarded path only
 * when the stub cannot run or gets no usable answer; the guard and its one
 * retry still bound that fallback.
 */
#define PSP_NETWORK_DNS_HARD_DEADLINE_MS 3000u
#define PSP_NETWORK_DNS_ATTEMPTS 2u

/* Retry only a lookup the guard had to stop: a resolver that answered (even
   "no such host") is not retried. */
static inline bool psp_network_dns_should_retry(
    bool resolved, bool stopped_by_guard, unsigned attempt)
{
    return !resolved && stopped_by_guard
        && attempt + 1u < PSP_NETWORK_DNS_ATTEMPTS;
}

/* Millisecond clocks wrap; compare by elapsed time, never by value. */
static inline bool psp_network_dns_lookup_overdue(
    bool active, uint32_t started_ms, uint32_t now_ms, bool stop_sent)
{
    return active && !stop_sent
        && (uint32_t) (now_ms - started_ms)
               >= PSP_NETWORK_DNS_HARD_DEADLINE_MS;
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

/*
 * One Wi-Fi join attempt sometimes ends or stalls on its own, and APCTL
 * never retries it. Four of about twenty consecutive PSP-3000 device runs
 * got no address in 45 s: three went JOINING -> DISCONNECTED 3-6 s after
 * Connect and then sat disconnected; one stayed in KEY_EXCHANGE. A healthy
 * join reaches GOT_IP in 4-5 s (key exchange about 3 s, DHCP about 1.3 s).
 * Rejoin instead of waiting out the deadline: reconnect a join that ended,
 * and disconnect (then reconnect) one stuck in a step.
 */
#define PSP_NETWORK_JOIN_ATTEMPTS 3u
/* Idle this long after an attempt ended before asking again. */
#define PSP_NETWORK_JOIN_RETRY_PAUSE_US UINT64_C(1000000)
/* A Connect that never left DISCONNECTED. */
#define PSP_NETWORK_JOIN_START_US UINT64_C(5000000)
/* JOINING, EAP or KEY_EXCHANGE held this long is stuck. */
#define PSP_NETWORK_JOIN_STEP_STALL_US UINT64_C(10000000)
/* GETTING_IP (DHCP) held this long is stuck. */
#define PSP_NETWORK_JOIN_DHCP_STALL_US UINT64_C(15000000)

typedef enum {
    PSP_NETWORK_JOIN_WAIT = 0,
    PSP_NETWORK_JOIN_CONNECT,
    PSP_NETWORK_JOIN_DISCONNECT
} PspNetworkJoinAction;

/* `attempts` counts Connect calls so far; `progressed` is whether this
   attempt was ever seen associating; the durations are since this attempt's
   Connect and since APCTL entered `state`. */
static inline PspNetworkJoinAction psp_network_join_action(
    int state, bool progressed, uint64_t attempt_us, uint64_t state_us,
    unsigned attempts)
{
    if (attempts == 0u || attempts >= PSP_NETWORK_JOIN_ATTEMPTS)
        return PSP_NETWORK_JOIN_WAIT;
    if (state == 0) {
        bool ended = progressed
            ? state_us >= PSP_NETWORK_JOIN_RETRY_PAUSE_US
            : attempt_us >= PSP_NETWORK_JOIN_START_US;
        return ended ? PSP_NETWORK_JOIN_CONNECT : PSP_NETWORK_JOIN_WAIT;
    }
    if (state == 3)
        return state_us >= PSP_NETWORK_JOIN_DHCP_STALL_US
            ? PSP_NETWORK_JOIN_DISCONNECT : PSP_NETWORK_JOIN_WAIT;
    if (state == 2 || state == 5 || state == 6)
        return state_us >= PSP_NETWORK_JOIN_STEP_STALL_US
            ? PSP_NETWORK_JOIN_DISCONNECT : PSP_NETWORK_JOIN_WAIT;
    return PSP_NETWORK_JOIN_WAIT;
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
