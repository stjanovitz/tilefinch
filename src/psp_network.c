#include "tilefinch/psp_network.h"
#include "psp_dns_stub.h"
#include "psp_network_policy.h"
#include "psp_utility_module_contract.h"
#include "tilefinch/psp_entropy.h"
#include "tilefinch/psp_log.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psputility_netparam.h>
#include <pspwlan.h>
#include <sys/socket.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PROFILE_QUERY_SECURITY (UINT32_C(1) << 0)
#define PROFILE_QUERY_STATIC_IP (UINT32_C(1) << 1)
#define PROFILE_QUERY_MANUAL_DNS (UINT32_C(1) << 2)
#define PROFILE_QUERY_PROXY (UINT32_C(1) << 3)

#define INTERFACE_QUERY_SECURITY (UINT32_C(1) << 0)
#define INTERFACE_QUERY_STRENGTH (UINT32_C(1) << 1)
#define INTERFACE_QUERY_CHANNEL (UINT32_C(1) << 2)
#define INTERFACE_QUERY_POWER_SAVE (UINT32_C(1) << 3)
#define INTERFACE_QUERY_PROXY (UINT32_C(1) << 4)
#define INTERFACE_QUERY_IP (UINT32_C(1) << 5)
#define INTERFACE_QUERY_SUBNET (UINT32_C(1) << 6)
#define INTERFACE_QUERY_GATEWAY (UINT32_C(1) << 7)
#define INTERFACE_QUERY_PRIMARY_DNS (UINT32_C(1) << 8)
#define INTERFACE_QUERY_SECONDARY_DNS (UINT32_C(1) << 9)

#define PSP_NETWORK_DISCONNECT_WAIT_US UINT64_C(250000)
#define PSP_NETWORK_DISCONNECT_POLL_US 5000u
#define PSP_NETWORK_PROFILE_LIMIT 100

/*
 * Name resolution is the one network primitive the application cannot bound
 * from the outside. The owned transport builds curl with
 * ENABLE_THREADED_RESOLVER=OFF, no c-ares, no DoH, no getaddrinfo and no
 * alarm(), so lib/hostip4.c takes its last branch and calls plain
 * gethostbyname(). That call runs to completion inside curl_easy_perform:
 * CURLOPT_CONNECTTIMEOUT_MS only starts counting once an address exists, and
 * the cancellation callback this browser installs is curl's progress
 * callback, which cannot fire while a resolve blocks. Whatever gethostbyname
 * costs is therefore a blind window in which the main thread cannot answer
 * Circle, cannot repaint, and cannot end a cooperate scope.
 *
 * PSPSDK's own gethostbyname is correct but is tuned for a resolver that has
 * all day. Disassembling gethostbyname.o out of libcglue.a shows it calling
 * sceNetResolverStartNtoA with a hardcoded two-second timeout and three
 * retries (`li a3,2` and `li t0,3`; psp-gcc's MIPS EABI passes the fifth
 * argument in $t0, not on the stack). An unresponsive DNS server therefore
 * costs up to four two-second rounds -- about eight seconds during which
 * nothing on this device can respond -- and the count is not configurable by
 * a caller.
 *
 * Define the symbol here instead and keep one retry: half the worst case,
 * still tolerant of a lost UDP round trip, and with the resolver scratch in
 * .bss rather than a kilobyte of a stack that is also carrying curl and Mbed
 * TLS. Our translation unit is a direct object on the link line, ahead of
 * libcglue.a, so this definition satisfies curl's reference and the archive
 * member is never pulled in -- a duplicate would have failed the link.
 * Nothing else in the program calls gethostbyname.
 *
 * The bound is deliberately transport-global; there is one resolver for the
 * process, and a faster, predictable DNS failure is the right behaviour for
 * page loads as well as for video. Resolved addresses land in curl's shared
 * DNS cache, so this cost is paid once per host rather than per request.
 *
 * The firmware call is now the fallback. gethostbyname first asks the access
 * point's DNS servers itself over one UDP socket (psp_dns_stub.h has the
 * schedule and the reasons), because the firmware resolver sometimes never
 * returns at all and a stopped lookup's retry can hang too. The firmware
 * path, with its guard and one retry, runs only when the stub cannot run or
 * every stub attempt went unanswered or unusable; an NXDOMAIN from the stub
 * is final.
 */
#define PSP_DNS_TIMEOUT_SECONDS 2u
#define PSP_DNS_RETRIES 1
#define PSP_DNS_RESOLVER_BUFFER_BYTES 1024u
#define PSP_DNS_ADDRESS_FAMILY_INET 2
#define PSP_DNS_ADDRESS_BYTES 4

static uint64_t psp_network_now_us(void)
{
    return (uint64_t) sceKernelGetSystemTimeWide();
}

/* One resolver scratch buffer, one hostent, one address: gethostbyname is a
   single-result interface by definition and this browser resolves from the
   single transport worker only. Static storage keeps a kilobyte off a stack
   that is also carrying curl, mbed TLS and the caller's frames. The stub
   resolver adds no shared buffers: its query, reply and socket live on the
   calling thread's stack (under 800 bytes against the worker's 256 KiB), and
   only its diagnostic counters below are process-wide. */
static unsigned char psp_dns_scratch[PSP_DNS_RESOLVER_BUFFER_BYTES]
    __attribute__((aligned(64)));
static struct in_addr psp_dns_address;
static char *psp_dns_address_list[2];
static char *psp_dns_aliases[1];
static char psp_dns_canonical_name[256];
static struct hostent psp_dns_hostent;

/* The lookup in flight, published for psp_network_dns_guard on the browser
   thread. The start time is written before the id and the id is withdrawn
   before the resolver is deleted, so the guard can only stop the lookup whose
   clock it read (or one that already finished, which firmware ignores). */
static volatile int psp_dns_active_resolver = -1;
static volatile uint32_t psp_dns_started_ms;
static volatile int psp_dns_stop_sent;
static volatile unsigned psp_dns_forced_stops;

/* Stub outcomes and its stand-down run. Plain counters: should two threads
   ever resolve at once, a lost increment costs one diagnostic count and at
   worst moves the stand-down by one lookup. */
static volatile unsigned psp_dns_stub_answered;
static volatile unsigned psp_dns_stub_retransmitted;
static volatile unsigned psp_dns_stub_nxdomain;
static volatile unsigned psp_dns_stub_fallbacks;
static volatile unsigned psp_dns_stub_silent_run;

static uint32_t psp_network_now_ms(void)
{
    return (uint32_t) (sceKernelGetSystemTimeWide() / 1000u);
}

void psp_network_dns_guard(void)
{
    int resolver_id = psp_dns_active_resolver;
    if (!psp_network_dns_lookup_overdue(
            resolver_id >= 0, psp_dns_started_ms, psp_network_now_ms(),
            psp_dns_stop_sent != 0)) return;
    psp_dns_stop_sent = 1;
    if (sceNetResolverStop(resolver_id) >= 0) psp_dns_forced_stops++;
}

unsigned psp_network_dns_forced_stops(void)
{
    return psp_dns_forced_stops;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
void psp_network_dns_stub_counters(PspNetworkDnsStubCounters *counters)
{
    if (counters == NULL) return;
    counters->answered = psp_dns_stub_answered;
    counters->retransmitted = psp_dns_stub_retransmitted;
    counters->nxdomain = psp_dns_stub_nxdomain;
    counters->fallbacks = psp_dns_stub_fallbacks;
    counters->stood_down =
        psp_dns_stub_stood_down(psp_dns_stub_silent_run);
}
#endif

/* Each wait is a sceNetInetPoll slice of at most this long, re-checked
   against the monotonic clock, so no single firmware wait can outlast the
   schedule by more than a slice. */
#define PSP_DNS_STUB_POLL_SLICE_MS 50u
/* A poll that returns at once with nothing to read (a pending socket error,
   or an early return) must not spin the transport worker for the rest of a
   window. */
#define PSP_DNS_STUB_IDLE_DELAY_US 2000
#define PSP_DNS_STUB_DRAIN_LIMIT 8u

typedef enum {
    PSP_DNS_LOOKUP_RESOLVED = 0,
    PSP_DNS_LOOKUP_NXDOMAIN,
    PSP_DNS_LOOKUP_FALLBACK
} PspDnsLookupOutcome;

typedef struct {
    struct sockaddr_in servers[PSP_DNS_STUB_MAX_SERVERS];
    unsigned server_count;
    unsigned transmissions;
    unsigned send_failures;
    unsigned last_server;
    bool heard;
    const char *reason;
} PspDnsStubRun;

/* The resolvers the access point handed out (or the profile's manual DNS),
   as APCTL reports them. PPSSPP's emulated AP may fill only the secondary.
   A duplicate secondary is dropped so the schedule's second server is a
   real alternative. */
static unsigned psp_dns_stub_servers(struct sockaddr_in *servers)
{
    static const int codes[PSP_DNS_STUB_MAX_SERVERS] = {
        PSP_NET_APCTL_INFO_PRIMDNS, PSP_NET_APCTL_INFO_SECDNS
    };
    unsigned count = 0u;
    for (unsigned index = 0u; index < PSP_DNS_STUB_MAX_SERVERS; index++) {
        union SceNetApctlInfo info;
        memset(&info, 0, sizeof(info));
        if (sceNetApctlGetInfo(codes[index], &info) < 0) continue;
        /* The firmware does not promise a terminator in the 16-byte
           field. */
        char text[sizeof(info.primaryDns) + 1u];
        memcpy(text, index == 0u ? info.primaryDns : info.secondaryDns,
               sizeof(info.primaryDns));
        text[sizeof(info.primaryDns)] = '\0';
        struct in_addr address;
        memset(&address, 0, sizeof(address));
        if (sceNetInetInetAton(text, &address) == 0
            || address.s_addr == 0u || address.s_addr == 0xFFFFFFFFu
            || (count == 1u
                && servers[0].sin_addr.s_addr == address.s_addr)) continue;
        struct sockaddr_in *server = &servers[count++];
        memset(server, 0, sizeof(*server));
        server->sin_len = (uint8_t) sizeof(*server);
        server->sin_family = AF_INET;
        server->sin_port = htons(PSP_DNS_STUB_PORT);
        server->sin_addr = address;
    }
    return count;
}

/* A reply counts only from port 53 of a server this lookup has already
   queried; a reply to an earlier transmission is welcome in any window. */
static int psp_dns_stub_source_server(
    const PspDnsStubRun *run, unsigned queried,
    const struct sockaddr_in *source)
{
    if (source->sin_family != AF_INET
        || source->sin_port != htons(PSP_DNS_STUB_PORT)) return -1;
    for (unsigned index = 0u; index < run->server_count; index++) {
        if ((queried & (1u << index)) != 0u
            && run->servers[index].sin_addr.s_addr
                   == source->sin_addr.s_addr) return (int) index;
    }
    return -1;
}

/* Query IDs come from the TLS entropy pool (tilefinch/psp_entropy.h)
   without waiting for it to be seeded: the first lookup precedes the first
   handshake, and an ID need only be unguessable off-path, which the seed
   file, boot context and per-call timing already make it. (libcglue's
   getentropy, used before, reseeds MT19937 from time() on every call, so
   its IDs were a function of the wall-clock second.) Each lookup also sends
   from a fresh socket, so a fresh source port, which is the other half of
   spoofing resistance. */
static uint16_t psp_dns_stub_query_id(void)
{
    uint16_t id = 0u;
    psp_entropy_fill_best_effort(&id, sizeof(id));
    return id;
}

static PspDnsLookupOutcome psp_dns_stub_resolve(
    const char *name, struct in_addr *address, PspDnsStubRun *run)
{
    uint8_t query[PSP_DNS_STUB_QUERY_BYTES];
    uint8_t reply[PSP_DNS_STUB_RESPONSE_BYTES];
    size_t query_bytes = psp_dns_stub_encode_query(
        query, sizeof(query), psp_dns_stub_query_id(), name);
    run->reason = "encode";
    if (query_bytes == 0u) return PSP_DNS_LOOKUP_FALLBACK;
    run->server_count = psp_dns_stub_servers(run->servers);
    run->reason = "no-server";
    if (run->server_count == 0u) return PSP_DNS_LOOKUP_FALLBACK;
    int socket_id = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
    run->reason = "socket";
    if (socket_id < 0) return PSP_DNS_LOOKUP_FALLBACK;

    PspDnsLookupOutcome outcome = PSP_DNS_LOOKUP_FALLBACK;
    unsigned step = 0u;
    unsigned dead = 0u;
    unsigned queried = 0u;
    unsigned server = 0u;
    uint32_t wait_ms = 0u;
    unsigned all_servers = (1u << run->server_count) - 1u;
    /* Non-blocking, so a readiness report that turns out empty cannot park
       the worker in recvfrom. */
    int enabled = 1;
    run->reason = "nonblock";
    if (sceNetInetSetsockopt(socket_id, SOL_SOCKET, SO_NONBLOCK, &enabled,
                             sizeof(enabled)) < 0) goto done;
    run->reason = "timeout";
    while (psp_dns_stub_next_send(&step, run->server_count, dead, &server,
                                  &wait_ms)) {
        int sent = (int) sceNetInetSendto(
            socket_id, query, query_bytes, 0,
            (const struct sockaddr *) &run->servers[server],
            sizeof(run->servers[server]));
        if (sent == (int) query_bytes) {
            queried |= 1u << server;
            run->transmissions++;
            run->last_server = server;
        } else {
            /* Still wait out the window if an earlier transmission's reply
               may yet arrive; with nothing outstanding there is nothing to
               wait for, so a host whose UDP path refuses every send costs
               no time before the firmware fallback. */
            run->send_failures++;
            if (queried == 0u) continue;
        }
        uint64_t deadline_us =
            psp_network_now_us() + (uint64_t) wait_ms * 1000u;
        for (;;) {
            uint64_t polled_us = psp_network_now_us();
            if (polled_us >= deadline_us || (dead & (1u << server)) != 0u)
                break;
            uint64_t left_ms = (deadline_us - polled_us + 999u) / 1000u;
            struct SceNetInetPollfd descriptor = {
                .fd = socket_id,
                .events = SCE_NET_INET_POLLIN
            };
            int ready = sceNetInetPoll(
                &descriptor, 1u,
                (int) (left_ms < PSP_DNS_STUB_POLL_SLICE_MS
                           ? left_ms : PSP_DNS_STUB_POLL_SLICE_MS));
            if (ready < 0) {
                run->reason = "poll";
                goto done;
            }
            unsigned drained = 0u;
            for (; ready > 0 && drained < PSP_DNS_STUB_DRAIN_LIMIT;
                 drained++) {
                struct sockaddr_in source;
                memset(&source, 0, sizeof(source));
                socklen_t source_bytes = sizeof(source);
                int received = (int) sceNetInetRecvfrom(
                    socket_id, reply, sizeof(reply), 0,
                    (struct sockaddr *) &source, &source_bytes);
                if (received <= 0) break;
                int from = psp_dns_stub_source_server(run, queried, &source);
                if (from < 0) continue;
                uint8_t bytes[4];
                PspDnsStubReply parsed = psp_dns_stub_parse_response(
                    reply, (size_t) received, query, query_bytes, bytes);
                if (parsed == PSP_DNS_STUB_IGNORE) continue;
                run->heard = true;
                run->last_server = (unsigned) from;
                if (parsed == PSP_DNS_STUB_ANSWER) {
                    memcpy(&address->s_addr, bytes, sizeof(bytes));
                    run->reason = "answer";
                    outcome = PSP_DNS_LOOKUP_RESOLVED;
                    goto done;
                }
                if (parsed == PSP_DNS_STUB_NXDOMAIN) {
                    run->reason = "nxdomain";
                    outcome = PSP_DNS_LOOKUP_NXDOMAIN;
                    goto done;
                }
                if (parsed == PSP_DNS_STUB_TRUNCATED) {
                    run->reason = "truncated";
                    goto done;
                }
                /* PSP_DNS_STUB_UNUSABLE: stop asking this server. */
                dead |= 1u << (unsigned) from;
                run->reason = "unusable";
                if ((dead & all_servers) == all_servers) goto done;
            }
            if (drained == 0u && psp_network_now_us() - polled_us < 1000u)
                sceKernelDelayThread(PSP_DNS_STUB_IDLE_DELAY_US);
        }
    }
done:
    (void) sceNetInetClose(socket_id);
    return outcome;
}

/* The firmware resolver, bounded from outside by psp_network_dns_guard and
   retried once when the guard had to stop it. */
static bool psp_dns_firmware_resolve(const char *name)
{
    bool resolved = false;
    for (unsigned attempt = 0; ; attempt++) {
        int resolver_id = -1;
        int created = sceNetResolverCreate(
            &resolver_id, psp_dns_scratch,
            (SceSize) sizeof(psp_dns_scratch));
        if (created < 0) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
            psp_log_printf("tilefinch-dns: host=%.64s attempt=%u "
                           "create=0x%08X\n", name, attempt,
                           (unsigned) created);
#endif
            return false;
        }
        psp_dns_started_ms = psp_network_now_ms();
        psp_dns_stop_sent = 0;
        __sync_synchronize();
        psp_dns_active_resolver = resolver_id;
        int started = sceNetResolverStartNtoA(
            resolver_id, name, &psp_dns_address,
            PSP_DNS_TIMEOUT_SECONDS, PSP_DNS_RETRIES);
        resolved = started >= 0;
        psp_dns_active_resolver = -1;
        __sync_synchronize();
        int deleted = sceNetResolverDelete(resolver_id);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (!resolved || deleted < 0 || psp_dns_stop_sent != 0) {
            psp_log_printf(
                "tilefinch-dns: host=%.64s attempt=%u resolver=%d "
                "start=0x%08X delete=0x%08X stopped=%d elapsed=%ums\n",
                name, attempt, resolver_id, (unsigned) started,
                (unsigned) deleted, psp_dns_stop_sent != 0 ? 1 : 0,
                (unsigned) (psp_network_now_ms() - psp_dns_started_ms));
        }
#else
        (void) deleted;
#endif
        if (!psp_network_dns_should_retry(
                resolved, psp_dns_stop_sent != 0, attempt)) break;
        memset(&psp_dns_address, 0, sizeof(psp_dns_address));
    }
    return resolved;
}

/* The stub first; the firmware only when the stub could not decide. False
   means the name does not resolve. */
static bool psp_dns_resolve(const char *name)
{
    if (!psp_dns_stub_owns_name(name)
        || psp_dns_stub_stood_down(psp_dns_stub_silent_run))
        return psp_dns_firmware_resolve(name);

    PspDnsStubRun run;
    memset(&run, 0, sizeof(run));
    struct in_addr address;
    memset(&address, 0, sizeof(address));
    uint64_t started_us = psp_network_now_us();
    PspDnsLookupOutcome outcome =
        psp_dns_stub_resolve(name, &address, &run);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* One line for any stub lookup that was not a first-try answer. */
    if (outcome != PSP_DNS_LOOKUP_RESOLVED || run.transmissions > 1u
        || run.send_failures != 0u) {
        psp_log_printf(
            "tilefinch-dns: stub host=%.64s outcome=%s servers=%u "
            "server=%u sent=%u send-failures=%u elapsed=%ums\n",
            name, run.reason, run.server_count, run.last_server,
            run.transmissions, run.send_failures,
            (unsigned) ((psp_network_now_us() - started_us) / 1000u));
    }
#else
    (void) started_us;
#endif
    if (run.transmissions > 1u) psp_dns_stub_retransmitted++;
    if (outcome == PSP_DNS_LOOKUP_RESOLVED) {
        psp_dns_stub_answered++;
        psp_dns_stub_silent_run = 0u;
        psp_dns_address = address;
        return true;
    }
    if (outcome == PSP_DNS_LOOKUP_NXDOMAIN) {
        psp_dns_stub_nxdomain++;
        psp_dns_stub_silent_run = 0u;
        return false;
    }
    psp_dns_stub_fallbacks++;
    bool resolved = psp_dns_firmware_resolve(name);
    /* Only a stub that transmitted and heard nothing is evidence against
       it; no servers or no socket cost nothing and prove nothing. */
    if (run.transmissions != 0u) {
        bool was_down = psp_dns_stub_stood_down(psp_dns_stub_silent_run);
        psp_dns_stub_silent_run = psp_dns_stub_silence_after(
            psp_dns_stub_silent_run, run.heard, resolved);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (!was_down && psp_dns_stub_stood_down(psp_dns_stub_silent_run))
            psp_log_printf("tilefinch-dns: stub stood down after %u "
                           "silent lookups\n", PSP_DNS_STUB_SILENT_LIMIT);
#else
        (void) was_down;
#endif
    }
    return resolved;
}

struct hostent *gethostbyname(const char *name)
{
    if (name == NULL || name[0] == '\0') return NULL;
    memset(&psp_dns_address, 0, sizeof(psp_dns_address));
    if (sceNetInetInetAton(name, &psp_dns_address) == 0
        && !psp_dns_resolve(name)) return NULL;
    snprintf(psp_dns_canonical_name, sizeof(psp_dns_canonical_name),
             "%s", name);
    psp_dns_address_list[0] = (char *) &psp_dns_address;
    psp_dns_address_list[1] = NULL;
    psp_dns_aliases[0] = NULL;
    psp_dns_hostent.h_name = psp_dns_canonical_name;
    psp_dns_hostent.h_aliases = psp_dns_aliases;
    psp_dns_hostent.h_addrtype = PSP_DNS_ADDRESS_FAMILY_INET;
    psp_dns_hostent.h_length = PSP_DNS_ADDRESS_BYTES;
    psp_dns_hostent.h_addr_list = psp_dns_address_list;
    psp_dns_hostent.h_addr = psp_dns_address_list[0];
    return &psp_dns_hostent;
}

static void psp_network_sample_memory(PspNetwork *network)
{
    size_t free_memory = (size_t) sceKernelTotalFreeMemSize();
    size_t maximum_block = (size_t) sceKernelMaxFreeMemSize();
    if (network->free_memory_minimum == 0
        || free_memory < network->free_memory_minimum) {
        network->free_memory_minimum = free_memory;
    }
    if (network->maximum_free_block_minimum == 0
        || maximum_block < network->maximum_free_block_minimum) {
        network->maximum_free_block_minimum = maximum_block;
    }
}

static PspNetworkStatus psp_network_finish_pump(
    PspNetwork *network, PspNetworkStatus phase, uint64_t started_us)
{
    uint64_t now_us = psp_network_now_us();
    uint64_t pump_us = now_us >= started_us ? now_us - started_us : 0;
    network->last_pump_us = pump_us;
    if (pump_us > network->maximum_pump_us) {
        network->maximum_pump_us = pump_us;
        network->maximum_pump_phase = phase;
    }
    if (phase >= PSP_NETWORK_IDLE && phase < PSP_NETWORK_STATUS_COUNT) {
        network->phase_pump_us[phase] += pump_us;
        network->phase_pump_calls[phase]++;
    }
    network->elapsed_us = now_us >= network->started_us
        ? now_us - network->started_us : 0;
    psp_network_sample_memory(network);
    if (network->status == PSP_NETWORK_READY) {
        network->free_memory_ready =
            (size_t) sceKernelTotalFreeMemSize();
        network->maximum_free_block_ready =
            (size_t) sceKernelMaxFreeMemSize();
    }
    psp_network_diagnostics_record(network);
    return network->status;
}

static void psp_network_query_profile(PspNetwork *network)
{
    static const struct {
        int parameter;
        uint32_t bit;
    } queries[] = {
        { PSP_NETPARAM_SECURE, PROFILE_QUERY_SECURITY },
        { PSP_NETPARAM_IS_STATIC_IP, PROFILE_QUERY_STATIC_IP },
        { PSP_NETPARAM_MANUAL_DNS, PROFILE_QUERY_MANUAL_DNS },
        { PSP_NETPARAM_USE_PROXY, PROFILE_QUERY_PROXY }
    };
    for (size_t index = 0; index < sizeof(queries) / sizeof(queries[0]);
         index++) {
        netData data;
        memset(&data, 0, sizeof(data));
        if (sceUtilityGetNetParam(
                network->profile_index, queries[index].parameter,
                &data) < 0) {
            network->profile_query_failure_mask |= queries[index].bit;
            continue;
        }
        network->profile_query_success_mask |= queries[index].bit;
        if (queries[index].parameter == PSP_NETPARAM_SECURE)
            network->profile_security_type = data.asUint;
        else if (queries[index].parameter == PSP_NETPARAM_IS_STATIC_IP)
            network->profile_static_ip = data.asUint != 0;
        else if (queries[index].parameter == PSP_NETPARAM_MANUAL_DNS)
            network->profile_manual_dns = data.asUint != 0;
        else if (queries[index].parameter == PSP_NETPARAM_USE_PROXY)
            network->profile_uses_proxy = data.asUint != 0;
    }
}

/* Saved PSP connection profiles are numbered from one, but deleting the
   configured/default slot can leave Tilefinch pointing at no profile at all.
   Firmware reports that case as PSP_NETPARAM_ERROR_BAD_NETCONF before the
   WLAN stack starts. On that specific error only, fall back to the first
   profile the firmware says is valid. This never changes a valid explicit
   selection and remains bounded even on corrupt profile storage. */
static int psp_network_check_profile(int profile, void *context)
{
    (void) context;
    return sceUtilityCheckNetParam(profile);
}

static bool psp_network_select_fallback_profile(PspNetwork *network)
{
    if (network == NULL) return false;
    int selected = psp_network_first_fallback_profile(
        network->requested_profile_index, PSP_NETWORK_PROFILE_LIMIT,
        psp_network_check_profile, NULL);
    if (selected == 0) return false;
    network->profile_index = selected;
    network->profile_fallback_used = true;
    return true;
}

int psp_network_choose_saved_profile(int current, int direction)
{
    return psp_network_next_saved_profile(
        current, PSP_NETWORK_PROFILE_LIMIT, direction,
        psp_network_check_profile, NULL);
}

bool psp_network_profile_is_saved(int profile)
{
    return profile >= 1 && profile <= PSP_NETWORK_PROFILE_LIMIT
        && sceUtilityCheckNetParam(profile) == 0;
}

bool psp_network_profile_ssid(
    int profile, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0u) return false;
    output[0] = '\0';
    if (!psp_network_profile_is_saved(profile)) return false;

    netData data;
    memset(&data, 0, sizeof(data));
    if (sceUtilityGetNetParam(profile, PSP_NETPARAM_SSID, &data) < 0)
        return false;
    data.asString[sizeof(data.asString) - 1u] = '\0';

    size_t written = 0u;
    for (size_t at = 0u; data.asString[at] != '\0'
         && written + 1u < output_size; at++) {
        unsigned char byte = (unsigned char) data.asString[at];
        /* Saved SSIDs are byte strings. Keep the chrome renderer boring and
           deterministic if firmware returns controls or non-UTF-8 bytes. */
        output[written++] = byte >= 0x20u && byte <= 0x7eu
            ? (char) byte : '?';
    }
    output[written] = '\0';
    return written != 0u;
}

static PspNetworkStatus psp_network_fail(PspNetwork *network, int result)
{
    network->failure_phase = network->status;
    network->native_result = result;
    uint64_t now_us = psp_network_now_us();
    network->elapsed_us = now_us >= network->started_us
        ? now_us - network->started_us : 0;
    network->status = PSP_NETWORK_FAILED;
    return network->status;
}

static void psp_network_join_started(
    PspNetwork *network, uint64_t now_us, bool associating)
{
    network->join_attempts++;
    network->join_attempt_started_us = now_us;
    network->join_state = network->apctl_state;
    network->join_state_started_us = now_us;
    network->join_progressed = associating;
    network->join_disconnect_requested = false;
}

/* Rejoin a join APCTL abandoned or stalled in (psp_network_join_action).
   Returns a negative firmware status only when a rejoin call fails. */
static int psp_network_join_step(PspNetwork *network, uint64_t now_us)
{
    int state = network->apctl_state;
    if (state != network->join_state) {
        network->join_state = state;
        network->join_state_started_us = now_us;
    }
    if (psp_network_apctl_state_is_associating(state))
        network->join_progressed = true;
    PspNetworkJoinAction action = psp_network_join_action(
        state, network->join_progressed,
        now_us - network->join_attempt_started_us,
        now_us - network->join_state_started_us, network->join_attempts);
    if (action == PSP_NETWORK_JOIN_DISCONNECT
        && !network->join_disconnect_requested) {
        int result = sceNetApctlDisconnect();
        if (result < 0) return result;
        network->join_disconnect_requested = true;
        network->join_resets++;
        return 0;
    }
    if (action == PSP_NETWORK_JOIN_CONNECT) {
        int result = sceNetApctlConnect(network->profile_index);
        if (result < 0) return result;
        psp_network_join_started(network, now_us, false);
    }
    return 0;
}

bool psp_network_begin(PspNetwork *network, int profile_index)
{
    if (network == NULL || profile_index <= 0) return false;
    memset(network, 0, sizeof(*network));
    network->requested_profile_index = profile_index;
    network->diagnostics_enabled = psp_network_diagnostics_enabled();
    network->profile_index = profile_index;
    network->apctl_state = -1;
    network->native_result = 0;
    network->started_us = psp_network_now_us();
    network->free_memory_start =
        (size_t) sceKernelTotalFreeMemSize();
    network->free_memory_minimum = network->free_memory_start;
    network->maximum_free_block_start =
        (size_t) sceKernelMaxFreeMemSize();
    network->maximum_free_block_minimum =
        network->maximum_free_block_start;
    network->maximum_pump_phase = PSP_NETWORK_IDLE;
    network->wlan_switch_state = -1;
    network->wlan_power_state = -1;
    network->status = PSP_NETWORK_CHECKING_PROFILE;
    return true;
}

PspNetworkStatus psp_network_pump(PspNetwork *network,
                                  uint64_t timeout_us)
{
    if (network == NULL) return PSP_NETWORK_FAILED;
    if (network->status == PSP_NETWORK_READY
        || network->status == PSP_NETWORK_FAILED
        || network->status == PSP_NETWORK_CANCELLED
        || network->status == PSP_NETWORK_IDLE) return network->status;
    uint64_t now_us = psp_network_now_us();
    network->elapsed_us = now_us >= network->started_us
        ? now_us - network->started_us : 0;
    network->pump_calls++;
    PspNetworkStatus phase = network->status;
    uint64_t pump_started_us = now_us;
    if (timeout_us != 0 && network->elapsed_us >= timeout_us) {
        (void) psp_network_fail(network, -1);
        return psp_network_finish_pump(
            network, phase, pump_started_us);
    }

    int result = 0;
    switch (network->status) {
    case PSP_NETWORK_CHECKING_PROFILE:
        result = sceUtilityCheckNetParam(network->profile_index);
        if ((uint32_t) result == (uint32_t) PSP_NETPARAM_ERROR_BAD_NETCONF
            && psp_network_select_fallback_profile(network)) {
            result = 0;
        }
        if (result < 0) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        psp_network_query_profile(network);
        network->status = PSP_NETWORK_LOADING_COMMON;
        break;
    case PSP_NETWORK_LOADING_COMMON:
        network->wlan_switch_state = sceWlanGetSwitchState();
        network->wlan_power_state = sceWlanDevIsPowerOn();
        /* The physical switch is a real precondition.  Device power is not:
           on a cold boot sceWlanDevIsPowerOn() remains false until the net
           modules/APCTL bring the radio up.  Rejecting that state here made
           the first connection attempt fail before the WLAN lamp could ever
           illuminate.  Keep the sample for diagnostics, then follow the
           PSPSDK connection order and let module/APCTL errors be authoritative. */
        if (network->wlan_switch_state <= 0) {
            (void) psp_network_fail(network, -2);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        PspUtilityModuleLoadDisposition common_disposition =
            psp_utility_load_net_module(
                PSP_NET_MODULE_COMMON, &result);
        if (common_disposition == PSP_UTILITY_MODULE_LOAD_FAILED) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        network->common_loaded = true;
        network->common_owned = psp_utility_module_load_owned(
            common_disposition);
        network->status = PSP_NETWORK_LOADING_INET;
        break;
    case PSP_NETWORK_LOADING_INET: {
        PspUtilityModuleLoadDisposition inet_disposition =
            psp_utility_load_net_module(
                PSP_NET_MODULE_INET, &result);
        if (inet_disposition == PSP_UTILITY_MODULE_LOAD_FAILED) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        network->inet_loaded = true;
        network->inet_owned = psp_utility_module_load_owned(
            inet_disposition);
        network->status = PSP_NETWORK_INITIALIZING_CORE;
        break;
    }
    case PSP_NETWORK_INITIALIZING_CORE:
        result = sceNetInit(0x20000, 0x2a, 0, 0x2a, 0);
        bool core_adopted = false;
        if (!psp_network_init_result_usable(
                PSP_NETWORK_INIT_CORE, result, &core_adopted)) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        if (core_adopted)
            network->initialization_adopted_mask |= UINT32_C(1) << 0;
        network->core_initialized = true;
        network->status = PSP_NETWORK_INITIALIZING_INET;
        break;
    case PSP_NETWORK_INITIALIZING_INET:
        result = sceNetInetInit();
        bool inet_adopted = false;
        if (!psp_network_init_result_usable(
                PSP_NETWORK_INIT_INET, result, &inet_adopted)) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        if (inet_adopted)
            network->initialization_adopted_mask |= UINT32_C(1) << 1;
        network->inet_initialized = true;
        network->status = PSP_NETWORK_INITIALIZING_RESOLVER;
        break;
    case PSP_NETWORK_INITIALIZING_RESOLVER:
        result = sceNetResolverInit();
        bool resolver_adopted = false;
        if (!psp_network_init_result_usable(
                PSP_NETWORK_INIT_RESOLVER, result, &resolver_adopted)) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        if (resolver_adopted)
            network->initialization_adopted_mask |= UINT32_C(1) << 2;
        network->resolver_initialized = true;
        network->status = PSP_NETWORK_INITIALIZING_APCTL;
        break;
    case PSP_NETWORK_INITIALIZING_APCTL:
        result = sceNetApctlInit(0x8000, 0x30);
        bool apctl_adopted = false;
        if (!psp_network_init_result_usable(
                PSP_NETWORK_INIT_APCTL, result, &apctl_adopted)) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        if (apctl_adopted)
            network->initialization_adopted_mask |= UINT32_C(1) << 3;
        network->apctl_initialized = true;
        network->status = PSP_NETWORK_CONNECTING;
        break;
    case PSP_NETWORK_CONNECTING:
        network->wlan_power_state = sceWlanDevIsPowerOn();
        result = sceNetApctlGetState(&network->apctl_state);
        if (result < 0) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        if (network->apctl_state == PSP_NET_APCTL_STATE_GOT_IP) {
            network->status = PSP_NETWORK_READY;
            break;
        }
        if (psp_network_apctl_state_is_associating(
                network->apctl_state)) {
            /* An APCTL service adopted after an incomplete teardown can
               still be finishing its prior association. Reissuing Connect
               in JOINING/GETTING_IP/EAP/KEY_EXCHANGE is rejected by real
               firmware; observe the existing transaction instead. */
            network->connect_started = true;
            psp_network_join_started(network, now_us, true);
            network->status = PSP_NETWORK_WAITING_FOR_IP;
            break;
        }
        result = sceNetApctlConnect(network->profile_index);
        if (result < 0) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        network->connect_started = true;
        psp_network_join_started(network, now_us, false);
        network->status = PSP_NETWORK_WAITING_FOR_IP;
        break;
    case PSP_NETWORK_WAITING_FOR_IP:
        network->wlan_power_state = sceWlanDevIsPowerOn();
        result = sceNetApctlGetState(&network->apctl_state);
        if (result < 0) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        if (network->apctl_state == PSP_NET_APCTL_STATE_GOT_IP) {
            network->status = PSP_NETWORK_READY;
            break;
        }
        result = psp_network_join_step(network, now_us);
        if (result < 0) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        break;
    default:
        (void) psp_network_fail(network, -3);
        return psp_network_finish_pump(
            network, phase, pump_started_us);
    }
    network->native_result = result;
    return psp_network_finish_pump(network, phase, pump_started_us);
}

void psp_network_mark_cancelled(PspNetwork *network)
{
    if (network == NULL || network->status == PSP_NETWORK_READY
        || network->status == PSP_NETWORK_FAILED
        || network->status == PSP_NETWORK_CANCELLED) return;
    uint64_t now_us = psp_network_now_us();
    network->elapsed_us = now_us >= network->started_us
        ? now_us - network->started_us : 0;
    network->status = PSP_NETWORK_CANCELLED;
}

static void psp_network_query_interface_field(
    PspNetworkInterfaceReport *report, int code, uint32_t bit,
    union SceNetApctlInfo *info)
{
    memset(info, 0, sizeof(*info));
    int result = sceNetApctlGetInfo(code, info);
    if (result < 0) {
        report->query_failure_mask |= bit;
        if (report->first_error == 0) report->first_error = result;
    } else {
        report->query_success_mask |= bit;
    }
}

bool psp_network_interface_report(
    const PspNetwork *network, PspNetworkInterfaceReport *report)
{
    if (network == NULL || report == NULL
        || network->status != PSP_NETWORK_READY) return false;
    memset(report, 0, sizeof(*report));
    union SceNetApctlInfo info;
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_SECURITY_TYPE,
        INTERFACE_QUERY_SECURITY, &info);
    report->security_type = info.securityType;
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_STRENGTH,
        INTERFACE_QUERY_STRENGTH, &info);
    report->signal_strength = info.strength;
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_CHANNEL,
        INTERFACE_QUERY_CHANNEL, &info);
    report->channel = info.channel;
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_POWER_SAVE,
        INTERFACE_QUERY_POWER_SAVE, &info);
    report->power_save = info.powerSave != 0;
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_USE_PROXY,
        INTERFACE_QUERY_PROXY, &info);
    report->uses_proxy = info.useProxy != 0;
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_IP, INTERFACE_QUERY_IP, &info);
    report->has_ip = info.ip[0] != '\0'
        && strcmp(info.ip, "0.0.0.0") != 0;
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_SUBNETMASK,
        INTERFACE_QUERY_SUBNET, &info);
    report->has_subnet = info.subNetMask[0] != '\0';
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_GATEWAY,
        INTERFACE_QUERY_GATEWAY, &info);
    report->has_gateway = info.gateway[0] != '\0';
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_PRIMDNS,
        INTERFACE_QUERY_PRIMARY_DNS, &info);
    report->has_primary_dns = info.primaryDns[0] != '\0';
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_SECDNS,
        INTERFACE_QUERY_SECONDARY_DNS, &info);
    report->has_secondary_dns = info.secondaryDns[0] != '\0';
    /* Some valid profiles (and PPSSPP's emulated AP) expose only the
       secondary resolver. Either resolver is sufficient for live traffic and
       for the chrome's WLAN-strength indicator. */
    return report->has_ip
        && (report->has_primary_dns || report->has_secondary_dns);
}

bool psp_network_resume_ready(
    PspNetwork *network, int *native_result)
{
    if (native_result != NULL) *native_result = 0;
    if (network == NULL || network->status != PSP_NETWORK_READY
        || !network->apctl_initialized) return false;
    int state = -1;
    int result = sceNetApctlGetState(&state);
    if (native_result != NULL) *native_result = result;
    network->apctl_state = state;
    network->wlan_switch_state = sceWlanGetSwitchState();
    network->wlan_power_state = sceWlanDevIsPowerOn();
    if (result < 0 || state != PSP_NET_APCTL_STATE_GOT_IP
        || network->wlan_switch_state <= 0
        || network->wlan_power_state <= 0) {
        return false;
    }
    PspNetworkInterfaceReport report;
    return psp_network_interface_report(network, &report);
}

bool psp_network_link_ready(PspNetwork *network, int *native_result)
{
    if (native_result != NULL) *native_result = 0;
    if (network == NULL || !network->apctl_initialized) return false;
    int state = -1;
    int result = sceNetApctlGetState(&state);
    if (native_result != NULL) *native_result = result;
    network->apctl_state = state;
    network->wlan_switch_state = sceWlanGetSwitchState();
    network->wlan_power_state = sceWlanDevIsPowerOn();
    return result >= 0 && state == PSP_NET_APCTL_STATE_GOT_IP
        && network->wlan_switch_state > 0
        && network->wlan_power_state > 0;
}

void psp_network_rejoin_begin(
    PspNetwork *network, PspNetworkRejoinOperation *operation)
{
    if (operation == NULL) return;
    memset(operation, 0, sizeof(*operation));
    if (network == NULL || !network->apctl_initialized
        || network->profile_index <= 0) {
        operation->phase = PSP_NETWORK_REJOIN_FAILED;
        operation->native_result = -1;
        return;
    }
    operation->started_us = psp_network_now_us();
    operation->phase_started_us = operation->started_us;
    operation->phase = PSP_NETWORK_REJOIN_LEAVING;
    network->disconnect_started = false;
    network->connect_started = false;
}

bool psp_network_rejoin_pump(
    PspNetwork *network, PspNetworkRejoinOperation *operation,
    uint64_t timeout_us)
{
    if (network == NULL || operation == NULL
        || operation->phase == PSP_NETWORK_REJOIN_COMPLETE
        || operation->phase == PSP_NETWORK_REJOIN_FAILED)
        return true;
    uint64_t now_us = psp_network_now_us();
    if (timeout_us != 0
        && now_us - operation->started_us >= timeout_us) {
        operation->phase = PSP_NETWORK_REJOIN_FAILED;
        operation->native_result = -1;
        return true;
    }
    int state = PSP_NET_APCTL_STATE_DISCONNECTED;
    int result = sceNetApctlGetState(&state);
    operation->native_result = result;
    if (result < 0) {
        operation->phase = PSP_NETWORK_REJOIN_FAILED;
        return true;
    }
    network->apctl_state = state;
    if (operation->phase == PSP_NETWORK_REJOIN_LEAVING) {
        if (state != PSP_NET_APCTL_STATE_DISCONNECTED) {
            if (!network->disconnect_started) {
                result = sceNetApctlDisconnect();
                operation->native_result = result;
                if (result < 0) {
                    operation->phase = PSP_NETWORK_REJOIN_FAILED;
                    return true;
                }
                network->disconnect_started = true;
            } else if (now_us - operation->phase_started_us
                       >= PSP_NETWORK_DISCONNECT_WAIT_US) {
                operation->phase = PSP_NETWORK_REJOIN_FAILED;
                operation->native_result = -2;
                return true;
            }
            return false;
        }
        result = sceNetApctlConnect(network->profile_index);
        operation->native_result = result;
        if (result < 0) {
            operation->phase = PSP_NETWORK_REJOIN_FAILED;
            return true;
        }
        network->disconnect_started = false;
        network->connect_started = true;
        network->join_attempts = 0;
        psp_network_join_started(network, now_us, false);
        operation->phase = PSP_NETWORK_REJOIN_WAITING_IP;
        operation->phase_started_us = now_us;
        return false;
    }
    if (state == PSP_NET_APCTL_STATE_GOT_IP) {
        network->status = PSP_NETWORK_READY;
        operation->phase = PSP_NETWORK_REJOIN_COMPLETE;
        return true;
    }
    /* A rejoin after resume fails the same ways a first join does. */
    result = psp_network_join_step(network, now_us);
    if (result < 0) {
        operation->native_result = result;
        operation->phase = PSP_NETWORK_REJOIN_FAILED;
        return true;
    }
    return false;
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
        operation->phase = PSP_NETWORK_SHUTDOWN_UNLOAD_INET;
        return false;
    case PSP_NETWORK_SHUTDOWN_UNLOAD_INET:
        if (network->inet_owned)
            psp_network_shutdown_result(
                report, UINT32_C(1) << 5,
                sceUtilityUnloadNetModule(PSP_NET_MODULE_INET));
        network->inet_owned = false;
        network->inet_loaded = false;
        operation->phase = PSP_NETWORK_SHUTDOWN_UNLOAD_COMMON;
        return false;
    case PSP_NETWORK_SHUTDOWN_UNLOAD_COMMON:
        if (network->common_owned)
            psp_network_shutdown_result(
                report, UINT32_C(1) << 6,
                sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON));
        network->common_owned = false;
        network->common_loaded = false;
        report->elapsed_us = psp_network_now_us() - operation->started_us;
        report->free_memory_after =
            (size_t) sceKernelTotalFreeMemSize();
        report->maximum_free_block_after =
            (size_t) sceKernelMaxFreeMemSize();
        memset(network, 0, sizeof(*network));
        operation->phase = PSP_NETWORK_SHUTDOWN_COMPLETE;
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

const char *psp_network_status_name(PspNetworkStatus status)
{
    switch (status) {
    case PSP_NETWORK_IDLE: return "idle";
    case PSP_NETWORK_CHECKING_PROFILE: return "check-profile";
    case PSP_NETWORK_LOADING_COMMON: return "load-common";
    case PSP_NETWORK_LOADING_INET: return "load-inet";
    case PSP_NETWORK_INITIALIZING_CORE: return "init-core";
    case PSP_NETWORK_INITIALIZING_INET: return "init-inet";
    case PSP_NETWORK_INITIALIZING_RESOLVER: return "init-resolver";
    case PSP_NETWORK_INITIALIZING_APCTL: return "init-apctl";
    case PSP_NETWORK_CONNECTING: return "connect";
    case PSP_NETWORK_WAITING_FOR_IP: return "waiting-ip";
    case PSP_NETWORK_READY: return "ready";
    case PSP_NETWORK_FAILED: return "failed";
    case PSP_NETWORK_CANCELLED: return "cancelled";
    case PSP_NETWORK_STATUS_COUNT: return "count";
    default: return "unknown";
    }
}
