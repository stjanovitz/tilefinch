#ifndef TILEFINCH_PSP_DNS_STUB_H
#define TILEFINCH_PSP_DNS_STUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The platform-free half of the PSP's DNS-over-UDP stub resolver: query
 * encoding, reply validation and parsing, and the retransmit schedule.
 * psp_network.c owns the socket, the clock and the server addresses.
 *
 * Why a stub at all: the firmware resolver (sceNetResolverStartNtoA) never
 * returns on about one PSP-3000 lookup in fifteen, whatever timeout it is
 * given. psp_network_dns_guard stops a hung lookup after three seconds and
 * gethostbyname retries once, but hangs can be consecutive: a device run
 * lost both attempts for an rr1---sn-*.googlevideo.com host (3 s each,
 * start=0x8041040E stopped=1) and the video never opened. Asking the access
 * point's DNS servers directly over one UDP socket takes the firmware
 * resolver out of the common path; every wait here is a bounded poll.
 *
 * Only what curl needs is implemented: one A question with RD set, the first
 * A record at the end of any CNAME chain in the answer section, and nothing
 * from the authority or additional sections. No EDNS is sent, so a server
 * keeps replies within 512 bytes and sets TC instead.
 */

#define PSP_DNS_STUB_PORT 53u
#define PSP_DNS_STUB_HEADER_BYTES 12u
/* RFC 1035: a name is at most 255 bytes on the wire, labels at most 63. */
#define PSP_DNS_STUB_NAME_BYTES 255u
#define PSP_DNS_STUB_LABEL_BYTES 63u
#define PSP_DNS_STUB_QUERY_BYTES \
    (PSP_DNS_STUB_HEADER_BYTES + PSP_DNS_STUB_NAME_BYTES + 4u)
#define PSP_DNS_STUB_RESPONSE_BYTES 512u
#define PSP_DNS_STUB_MAX_SERVERS 2u
/* Compression pointers may only point backwards, but labels read after a
   jump move forwards again, so a crafted packet can still cycle. Both the
   jump count and the 255-byte name bound end such a walk. */
#define PSP_DNS_STUB_POINTER_JUMPS 16u
#define PSP_DNS_STUB_CNAME_HOPS 8u

#define PSP_DNS_STUB_TYPE_A 1u
#define PSP_DNS_STUB_TYPE_CNAME 5u
#define PSP_DNS_STUB_CLASS_IN 1u
#define PSP_DNS_STUB_RCODE_NXDOMAIN 3u

/*
 * Retransmit schedule. Healthy firmware lookups finished in 7-61 ms on every
 * device run, so a first wait of 400 ms is several times the slowest healthy
 * answer yet short enough that a lost datagram costs little before the other
 * server is asked. The second round doubles the wait to cover an access
 * point that must recurse to an authoritative server for an uncached name
 * (googlevideo hosts are rarely cached). A reply to an earlier transmission
 * is accepted in any later window, so a slow primary still wins while the
 * secondary is being asked.
 *
 *   two servers: primary 400, secondary 400, primary 800, secondary 800 ms
 *   one server:  400, 800, 1200 ms
 *
 * Both total 2400 ms, under the 3 s PSP_NETWORK_DNS_HARD_DEADLINE_MS a
 * single firmware attempt is allowed, so the stub can never cost more than
 * one stopped firmware lookup did.
 */
#define PSP_DNS_STUB_BUDGET_MS 2400u

static inline bool psp_dns_stub_schedule(
    unsigned step, unsigned servers, unsigned *server, uint32_t *wait_ms)
{
    static const uint16_t two_servers[] = { 400u, 400u, 800u, 800u };
    static const uint16_t one_server[] = { 400u, 800u, 1200u };
    if (server == NULL || wait_ms == NULL) return false;
    if (servers >= 2u) {
        if (step >= sizeof(two_servers) / sizeof(two_servers[0]))
            return false;
        *server = step % 2u;
        *wait_ms = two_servers[step];
        return true;
    }
    if (servers == 0u
        || step >= sizeof(one_server) / sizeof(one_server[0])) return false;
    *server = 0u;
    *wait_ms = one_server[step];
    return true;
}

/* The next transmission, skipping servers that already answered without an
   address (dead_mask bit per server index). False ends the stub. */
static inline bool psp_dns_stub_next_send(
    unsigned *step, unsigned servers, unsigned dead_mask,
    unsigned *server, uint32_t *wait_ms)
{
    if (step == NULL) return false;
    while (psp_dns_stub_schedule(*step, servers, server, wait_ms)) {
        (*step)++;
        if ((dead_mask & (1u << *server)) == 0u) return true;
    }
    return false;
}

/* The stub is for names on the public DNS. Single-label names (localhost,
   an intranet host) and RFC 6761 *.localhost names keep going straight to
   the firmware resolver, exactly as before the stub existed: PPSSPP maps
   that resolver to the host's, which is how local fixtures are reached, and
   a public server's NXDOMAIN for them would be final. */
static inline bool psp_dns_stub_owns_name(const char *name)
{
    static const char local_suffix[] = ".localhost";
    if (name == NULL) return false;
    size_t length = 0u;
    bool interior_dot = false;
    for (; name[length] != '\0'; length++) {
        if (name[length] == '.' && length > 0u && name[length + 1u] != '\0')
            interior_dot = true;
    }
    if (!interior_dot) return false;
    size_t trimmed = name[length - 1u] == '.' ? length - 1u : length;
    size_t suffix = sizeof(local_suffix) - 1u;
    if (trimmed >= suffix) {
        bool local = true;
        for (size_t at = 0u; at < suffix && local; at++) {
            char c = name[trimmed - suffix + at];
            if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
            local = c == local_suffix[at];
        }
        if (local) return false;
    }
    return true;
}

/* Encode one recursive A/IN question. Returns the packet length, or zero for
   a name DNS cannot carry (empty or oversized labels, over 255 bytes). One
   trailing dot is accepted as the root. */
static inline size_t psp_dns_stub_encode_query(
    uint8_t *out, size_t capacity, uint16_t id, const char *name)
{
    if (out == NULL || name == NULL || name[0] == '\0'
        || capacity < PSP_DNS_STUB_HEADER_BYTES + 6u) return 0u;
    out[0] = (uint8_t) (id >> 8);
    out[1] = (uint8_t) id;
    out[2] = 0x01u; /* RD: the access point's server does the recursion */
    out[3] = 0x00u;
    out[4] = 0x00u;
    out[5] = 0x01u; /* QDCOUNT */
    for (size_t byte = 6u; byte < PSP_DNS_STUB_HEADER_BYTES; byte++)
        out[byte] = 0u;
    size_t at = PSP_DNS_STUB_HEADER_BYTES;
    size_t label_at = at++;
    for (const char *cursor = name; ; cursor++) {
        if (*cursor != '.' && *cursor != '\0') {
            if (at - label_at > PSP_DNS_STUB_LABEL_BYTES
                || capacity - at <= 5u) return 0u;
            out[at++] = (uint8_t) *cursor;
            continue;
        }
        size_t label = at - label_at - 1u;
        if (label == 0u) {
            /* Only "name." may end in an empty label. */
            if (*cursor != '\0' || cursor == name || label_at
                    == PSP_DNS_STUB_HEADER_BYTES) return 0u;
            at = label_at;
            break;
        }
        out[label_at] = (uint8_t) label;
        if (*cursor == '\0') break;
        label_at = at++;
    }
    if (capacity - at < 5u
        || at + 1u - PSP_DNS_STUB_HEADER_BYTES > PSP_DNS_STUB_NAME_BYTES)
        return 0u;
    out[at++] = 0u; /* root */
    out[at++] = 0u;
    out[at++] = PSP_DNS_STUB_TYPE_A;
    out[at++] = 0u;
    out[at++] = PSP_DNS_STUB_CLASS_IN;
    return at;
}

static inline unsigned psp_dns_stub_read16(const uint8_t *bytes)
{
    return ((unsigned) bytes[0] << 8) | bytes[1];
}

/* The offset just past a name in a record stream (a pointer ends it), or
   zero when the name runs off the packet or uses a reserved label type. */
static inline size_t psp_dns_stub_skip_name(
    const uint8_t *packet, size_t length, size_t offset)
{
    while (offset < length) {
        unsigned label = packet[offset];
        if (label == 0u) return offset + 1u;
        if ((label & 0xC0u) == 0xC0u)
            return length - offset >= 2u ? offset + 2u : 0u;
        if ((label & 0xC0u) != 0u) return 0u;
        offset += 1u + label;
    }
    return 0u;
}

typedef struct {
    const uint8_t *packet;
    size_t length;
    size_t offset;
    size_t name_bytes;
    unsigned jumps;
} PspDnsStubNameCursor;

/* Yields the next label of a possibly compressed name: its length (zero for
   the root) with *label pointing at its bytes, or -1 when malformed. */
static inline int psp_dns_stub_next_label(
    PspDnsStubNameCursor *cursor, const uint8_t **label)
{
    for (;;) {
        if (cursor->offset >= cursor->length) return -1;
        unsigned byte = cursor->packet[cursor->offset];
        if ((byte & 0xC0u) == 0xC0u) {
            if (cursor->length - cursor->offset < 2u) return -1;
            size_t target = ((size_t) (byte & 0x3Fu) << 8)
                | cursor->packet[cursor->offset + 1u];
            if (target >= cursor->offset
                || ++cursor->jumps > PSP_DNS_STUB_POINTER_JUMPS) return -1;
            cursor->offset = target;
            continue;
        }
        if ((byte & 0xC0u) != 0u
            || byte >= cursor->length - cursor->offset) return -1;
        cursor->name_bytes += 1u + byte;
        if (cursor->name_bytes > PSP_DNS_STUB_NAME_BYTES) return -1;
        *label = cursor->packet + cursor->offset + 1u;
        cursor->offset += 1u + byte;
        return (int) byte;
    }
}

/* DNS names compare ASCII case-insensitively (servers may echo 0x20
   mixed case). A malformed name equals nothing. */
static inline bool psp_dns_stub_names_equal(
    const uint8_t *a_packet, size_t a_length, size_t a_offset,
    const uint8_t *b_packet, size_t b_length, size_t b_offset)
{
    PspDnsStubNameCursor a = { a_packet, a_length, a_offset, 0u, 0u };
    PspDnsStubNameCursor b = { b_packet, b_length, b_offset, 0u, 0u };
    for (;;) {
        const uint8_t *a_label = NULL;
        const uint8_t *b_label = NULL;
        int a_bytes = psp_dns_stub_next_label(&a, &a_label);
        int b_bytes = psp_dns_stub_next_label(&b, &b_label);
        if (a_bytes < 0 || a_bytes != b_bytes) return false;
        if (a_bytes == 0) return true;
        for (int at = 0; at < a_bytes; at++) {
            unsigned x = a_label[at];
            unsigned y = b_label[at];
            if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
            if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
            if (x != y) return false;
        }
    }
}

typedef enum {
    /* Not a reply to this query (wrong ID, not a response, a different
       question, malformed header): keep waiting for the real one. */
    PSP_DNS_STUB_IGNORE = 0,
    /* The first A record at the end of the CNAME chain was written. */
    PSP_DNS_STUB_ANSWER,
    /* RCODE 3: the name does not exist. Definitive; the firmware resolver
       asks the same servers and would only repeat it. */
    PSP_DNS_STUB_NXDOMAIN,
    /* TC: the answer needs TCP, which the stub does not speak. */
    PSP_DNS_STUB_TRUNCATED,
    /* This server answered without a usable address (SERVFAIL, REFUSED,
       no A record, a broken or overlong chain, malformed records): stop
       asking it. */
    PSP_DNS_STUB_UNUSABLE
} PspDnsStubReply;

/* Validate a datagram against the query that was sent (its ID and question
   are taken from `query`) and extract the address, network byte order. The
   caller has already matched the source address and port to a server. */
static inline PspDnsStubReply psp_dns_stub_parse_response(
    const uint8_t *response, size_t length,
    const uint8_t *query, size_t query_length, uint8_t address[4])
{
    if (response == NULL || query == NULL || address == NULL
        || length < PSP_DNS_STUB_HEADER_BYTES
        || query_length < PSP_DNS_STUB_HEADER_BYTES
        || response[0] != query[0] || response[1] != query[1])
        return PSP_DNS_STUB_IGNORE;
    unsigned flags = psp_dns_stub_read16(response + 2u);
    /* QR set, standard query opcode, exactly our one question. */
    if ((flags & 0x8000u) == 0u || (flags & 0x7800u) != 0u
        || psp_dns_stub_read16(response + 4u) != 1u)
        return PSP_DNS_STUB_IGNORE;
    size_t question = PSP_DNS_STUB_HEADER_BYTES;
    size_t question_end = psp_dns_stub_skip_name(response, length, question);
    if (question_end == 0u || length - question_end < 4u
        || !psp_dns_stub_names_equal(response, length, question,
                                     query, query_length, question)
        || psp_dns_stub_read16(response + question_end)
               != PSP_DNS_STUB_TYPE_A
        || psp_dns_stub_read16(response + question_end + 2u)
               != PSP_DNS_STUB_CLASS_IN)
        return PSP_DNS_STUB_IGNORE;
    if ((flags & 0x0200u) != 0u) return PSP_DNS_STUB_TRUNCATED;
    unsigned rcode = flags & 0x000Fu;
    if (rcode == PSP_DNS_STUB_RCODE_NXDOMAIN) return PSP_DNS_STUB_NXDOMAIN;
    if (rcode != 0u) return PSP_DNS_STUB_UNUSABLE;

    unsigned answers = psp_dns_stub_read16(response + 6u);
    size_t records = question_end + 4u;
    /* The name being resolved, as an offset into the reply: the question,
       then each CNAME target in turn. Canonical order resolves in one pass;
       further passes cover servers that list the chain out of order. */
    size_t target = question;
    unsigned hops = 0u;
    for (;;) {
        bool followed = false;
        size_t at = records;
        for (unsigned index = 0u; index < answers; index++) {
            size_t owner = at;
            size_t fixed = psp_dns_stub_skip_name(response, length, at);
            if (fixed == 0u || length - fixed < 10u)
                return PSP_DNS_STUB_UNUSABLE;
            unsigned type = psp_dns_stub_read16(response + fixed);
            unsigned record_class =
                psp_dns_stub_read16(response + fixed + 2u);
            size_t data = fixed + 10u;
            size_t data_bytes = psp_dns_stub_read16(response + fixed + 8u);
            if (data_bytes > length - data) return PSP_DNS_STUB_UNUSABLE;
            at = data + data_bytes;
            if (record_class != PSP_DNS_STUB_CLASS_IN
                || !psp_dns_stub_names_equal(response, length, owner,
                                             response, length, target))
                continue;
            if (type == PSP_DNS_STUB_TYPE_A && data_bytes == 4u) {
                for (unsigned byte = 0u; byte < 4u; byte++)
                    address[byte] = response[data + byte];
                return PSP_DNS_STUB_ANSWER;
            }
            if (type == PSP_DNS_STUB_TYPE_CNAME) {
                size_t end = psp_dns_stub_skip_name(response, length, data);
                if (end == 0u || end - data > data_bytes
                    || ++hops > PSP_DNS_STUB_CNAME_HOPS)
                    return PSP_DNS_STUB_UNUSABLE;
                target = data;
                followed = true;
            }
        }
        /* Nothing left to follow: NODATA, or a chain whose final address
           the server did not include. */
        if (!followed) return PSP_DNS_STUB_UNUSABLE;
    }
}

/*
 * Stand-down. The stub has not yet run on hardware; if its socket path
 * turns out to be deaf somewhere (a firmware quirk, an emulator without
 * outside UDP) every lookup would pay the full 2.4 s budget before the
 * firmware fallback. A lookup the stub heard nothing for but the firmware
 * then resolved is evidence against the stub; one it heard any valid reply
 * for clears the count; a lookup both failed says nothing about the stub.
 * After three consecutive strikes gethostbyname stops using the stub for
 * the rest of the process.
 */
#define PSP_DNS_STUB_SILENT_LIMIT 3u

static inline unsigned psp_dns_stub_silence_after(
    unsigned silent_run, bool heard_reply, bool firmware_resolved)
{
    if (heard_reply) return 0u;
    if (firmware_resolved && silent_run < PSP_DNS_STUB_SILENT_LIMIT)
        return silent_run + 1u;
    return silent_run;
}

static inline bool psp_dns_stub_stood_down(unsigned silent_run)
{
    return silent_run >= PSP_DNS_STUB_SILENT_LIMIT;
}

#endif
