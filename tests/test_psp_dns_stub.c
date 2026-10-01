#include "../src/psp_dns_stub.h"
#include "../src/psp_network_policy.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

typedef struct {
    uint8_t bytes[700];
    size_t length;
} Packet;

static void put8(Packet *packet, unsigned value)
{
    packet->bytes[packet->length++] = (uint8_t) value;
}

static void put16(Packet *packet, unsigned value)
{
    put8(packet, value >> 8);
    put8(packet, value & 0xFFu);
}

/* An uncompressed name; returns where it starts so records can point at
   it. */
static size_t put_name(Packet *packet, const char *dotted)
{
    size_t start = packet->length;
    while (*dotted != '\0') {
        const char *end = strchr(dotted, '.');
        size_t label = end != NULL ? (size_t) (end - dotted)
                                   : strlen(dotted);
        put8(packet, (unsigned) label);
        memcpy(packet->bytes + packet->length, dotted, label);
        packet->length += label;
        dotted += label;
        if (*dotted == '.') dotted++;
    }
    put8(packet, 0u);
    return start;
}

/* Labels followed by a compression pointer to an earlier name. */
static size_t put_prefix_then_pointer(
    Packet *packet, const char *label, size_t target)
{
    size_t start = packet->length;
    size_t length = strlen(label);
    put8(packet, (unsigned) length);
    memcpy(packet->bytes + packet->length, label, length);
    packet->length += length;
    put16(packet, 0xC000u | (unsigned) target);
    return start;
}

static void put_pointer(Packet *packet, size_t target)
{
    put16(packet, 0xC000u | (unsigned) target);
}

static void put_record(Packet *packet, unsigned type, unsigned data_bytes)
{
    put16(packet, type);
    put16(packet, PSP_DNS_STUB_CLASS_IN);
    put16(packet, 0u);
    put16(packet, 300u); /* TTL */
    put16(packet, data_bytes);
}

static void put_a(Packet *packet, unsigned a, unsigned b, unsigned c,
                  unsigned d)
{
    put_record(packet, PSP_DNS_STUB_TYPE_A, 4u);
    put8(packet, a);
    put8(packet, b);
    put8(packet, c);
    put8(packet, d);
}

/* Start a reply by echoing the query header and question. */
static void begin_reply(Packet *packet, const uint8_t *query,
                        size_t query_length, unsigned flags,
                        unsigned answers)
{
    memcpy(packet->bytes, query, query_length);
    packet->length = query_length;
    packet->bytes[2] = (uint8_t) (flags >> 8);
    packet->bytes[3] = (uint8_t) flags;
    packet->bytes[6] = (uint8_t) (answers >> 8);
    packet->bytes[7] = (uint8_t) answers;
}

/* QR, RD, RA: an ordinary recursive answer. */
#define REPLY_OK 0x8180u

static PspDnsStubReply parse(const Packet *packet, const uint8_t *query,
                             size_t query_length, uint8_t address[4])
{
    return psp_dns_stub_parse_response(
        packet->bytes, packet->length, query, query_length, address);
}

static int test_encoding(void)
{
    uint8_t query[PSP_DNS_STUB_QUERY_BYTES];
    size_t length = psp_dns_stub_encode_query(
        query, sizeof(query), 0xBEEFu, "www.example.com");
    static const uint8_t expected[] = {
        0xBE, 0xEF, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 3, 'w', 'w', 'w', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3,
        'c', 'o', 'm', 0, 0x00, 0x01, 0x00, 0x01
    };
    CHECK(length == sizeof(expected));
    CHECK(memcmp(query, expected, sizeof(expected)) == 0);

    /* One trailing dot is the root, not an empty label. */
    uint8_t dotted[PSP_DNS_STUB_QUERY_BYTES];
    CHECK(psp_dns_stub_encode_query(
              dotted, sizeof(dotted), 0xBEEFu, "www.example.com.")
          == sizeof(expected));
    CHECK(memcmp(dotted, expected, sizeof(expected)) == 0);

    CHECK(psp_dns_stub_encode_query(query, sizeof(query), 1u, "") == 0u);
    CHECK(psp_dns_stub_encode_query(query, sizeof(query), 1u, ".") == 0u);
    CHECK(psp_dns_stub_encode_query(query, sizeof(query), 1u, "a..b")
          == 0u);
    CHECK(psp_dns_stub_encode_query(query, sizeof(query), 1u, ".a") == 0u);
    CHECK(psp_dns_stub_encode_query(query, sizeof(query), 1u, "a.b..")
          == 0u);
    CHECK(psp_dns_stub_encode_query(query, sizeof(query), 1u, NULL) == 0u);
    /* The header alone does not fit a question. */
    CHECK(psp_dns_stub_encode_query(query, 16u, 1u, "a.b") == 0u);
    CHECK(psp_dns_stub_encode_query(query, 20u, 1u, "a.b") == 0u);
    CHECK(psp_dns_stub_encode_query(query, 21u, 1u, "a.b") == 21u);

    /* Labels stop at 63 bytes. */
    char name[400];
    memset(name, 'a', 63u);
    strcpy(name + 63, ".com");
    CHECK(psp_dns_stub_encode_query(query, sizeof(query), 1u, name) != 0u);
    memset(name, 'a', 64u);
    strcpy(name + 64, ".com");
    CHECK(psp_dns_stub_encode_query(query, sizeof(query), 1u, name) == 0u);

    /* Names stop at 255 wire bytes, a 253-character dotted name: labels of
       63, 63, 63 and 62 bytes (256 on the wire) are too long; 63, 63, 63
       and 61 fit exactly. */
    size_t at = 0u;
    for (unsigned label = 0u; label < 4u; label++) {
        if (label != 0u) name[at++] = '.';
        memset(name + at, 'b', 63u);
        at += 63u;
    }
    name[at - 1u] = '\0';
    CHECK(psp_dns_stub_encode_query(query, sizeof(query), 1u, name) == 0u);
    name[at - 2u] = '\0';
    CHECK(psp_dns_stub_encode_query(query, sizeof(query), 1u, name)
          == PSP_DNS_STUB_HEADER_BYTES + PSP_DNS_STUB_NAME_BYTES + 4u);
    return 0;
}

static int test_answers(void)
{
    uint8_t query[PSP_DNS_STUB_QUERY_BYTES];
    size_t query_length = psp_dns_stub_encode_query(
        query, sizeof(query), 0x1234u,
        "rr1---sn-jxou0gtapo3-nx5e.googlevideo.com");
    CHECK(query_length != 0u);
    size_t question = PSP_DNS_STUB_HEADER_BYTES;
    uint8_t address[4] = {0};
    Packet reply;

    /* A normal answer, owner compressed back to the question. */
    begin_reply(&reply, query, query_length, REPLY_OK, 1u);
    put_pointer(&reply, question);
    put_a(&reply, 173u, 194u, 5u, 70u);
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_ANSWER);
    CHECK(address[0] == 173u && address[1] == 194u && address[2] == 5u
          && address[3] == 70u);

    /* The owner spelled out in full, in 0x20 mixed case. */
    begin_reply(&reply, query, query_length, REPLY_OK, 1u);
    put_name(&reply, "RR1---SN-jxou0gtapo3-NX5E.GoogleVideo.com");
    put_a(&reply, 10u, 0u, 0u, 1u);
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_ANSWER && address[3] == 1u);

    /* The question echo is case-insensitive too. */
    Packet echoed;
    begin_reply(&echoed, query, query_length, REPLY_OK, 1u);
    echoed.bytes[question + 1u] = 'R';
    echoed.bytes[question + 2u] = 'R';
    put_pointer(&echoed, question);
    put_a(&echoed, 10u, 0u, 0u, 2u);
    CHECK(parse(&echoed, query, query_length, address)
          == PSP_DNS_STUB_ANSWER && address[3] == 2u);

    /* Records of other types and classes, and an A record of the wrong
       length, are passed over for the first usable A record. */
    begin_reply(&reply, query, query_length, REPLY_OK, 4u);
    put_pointer(&reply, question);
    put_record(&reply, 28u, 16u); /* AAAA */
    for (unsigned byte = 0u; byte < 16u; byte++) put8(&reply, 0u);
    put_pointer(&reply, question);
    put_record(&reply, PSP_DNS_STUB_TYPE_A, 5u);
    for (unsigned byte = 0u; byte < 5u; byte++) put8(&reply, 9u);
    put_pointer(&reply, question);
    put16(&reply, PSP_DNS_STUB_TYPE_A);
    put16(&reply, 3u); /* CHAOS */
    put16(&reply, 0u);
    put16(&reply, 60u);
    put16(&reply, 4u);
    put8(&reply, 9u); put8(&reply, 9u); put8(&reply, 9u); put8(&reply, 9u);
    put_pointer(&reply, question);
    put_a(&reply, 192u, 0u, 2u, 7u);
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_ANSWER && address[0] == 192u && address[3] == 7u);

    /* A record for some other name does not answer this one. */
    begin_reply(&reply, query, query_length, REPLY_OK, 1u);
    put_name(&reply, "attacker.example");
    put_a(&reply, 6u, 6u, 6u, 6u);
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_UNUSABLE);
    return 0;
}

static int test_cname_chains(void)
{
    uint8_t query[PSP_DNS_STUB_QUERY_BYTES];
    size_t query_length = psp_dns_stub_encode_query(
        query, sizeof(query), 0x4242u, "www.example.com");
    CHECK(query_length != 0u);
    size_t question = PSP_DNS_STUB_HEADER_BYTES;
    /* "example.com" inside the question: skip "www" (1 + 3 bytes). */
    size_t example = question + 4u;
    uint8_t address[4] = {0};
    Packet reply;

    /* www.example.com CNAME edge.example.com (compressed onto the
       question), then edge.example.com A (compressed onto the CNAME's
       data): the usual shape of a CDN answer. */
    begin_reply(&reply, query, query_length, REPLY_OK, 2u);
    put_pointer(&reply, question);
    put_record(&reply, PSP_DNS_STUB_TYPE_CNAME, 7u);
    size_t edge = put_prefix_then_pointer(&reply, "edge", example);
    put_pointer(&reply, edge);
    put_a(&reply, 203u, 0u, 113u, 9u);
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_ANSWER && address[0] == 203u && address[3] == 9u);

    /* The same chain listed out of order still resolves. */
    Packet shuffled;
    begin_reply(&shuffled, query, query_length, REPLY_OK, 2u);
    size_t target = put_name(&shuffled, "edge.example.net");
    put_a(&shuffled, 198u, 51u, 100u, 4u);
    put_pointer(&shuffled, question);
    put_record(&shuffled, PSP_DNS_STUB_TYPE_CNAME, 2u);
    put_pointer(&shuffled, target);
    CHECK(parse(&shuffled, query, query_length, address)
          == PSP_DNS_STUB_ANSWER && address[0] == 198u && address[3] == 4u);

    /* An A record for the alias itself is not the answer once the question
       has been followed to its canonical name... */
    begin_reply(&reply, query, query_length, REPLY_OK, 3u);
    put_pointer(&reply, question);
    put_record(&reply, PSP_DNS_STUB_TYPE_CNAME, 7u);
    edge = put_prefix_then_pointer(&reply, "edge", example);
    put_name(&reply, "other.example.com");
    put_a(&reply, 1u, 1u, 1u, 1u);
    put_pointer(&reply, edge);
    put_a(&reply, 2u, 2u, 2u, 2u);
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_ANSWER && address[0] == 2u);

    /* ...and a chain whose end has no address is not an answer. */
    begin_reply(&reply, query, query_length, REPLY_OK, 1u);
    put_pointer(&reply, question);
    put_record(&reply, PSP_DNS_STUB_TYPE_CNAME, 7u);
    put_prefix_then_pointer(&reply, "edge", example);
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_UNUSABLE);

    /* A CNAME loop (www -> loop -> www) ends at the hop bound. */
    begin_reply(&reply, query, query_length, REPLY_OK, 2u);
    put_pointer(&reply, question);
    put_record(&reply, PSP_DNS_STUB_TYPE_CNAME, 7u);
    size_t loop = put_prefix_then_pointer(&reply, "loop", example);
    put_pointer(&reply, loop);
    put_record(&reply, PSP_DNS_STUB_TYPE_CNAME, 2u);
    put_pointer(&reply, question);
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_UNUSABLE);

    /* Exactly PSP_DNS_STUB_CNAME_HOPS aliases resolve; one more does not.
       Each alias is "hN" prefixed onto example.com. */
    for (unsigned hops = PSP_DNS_STUB_CNAME_HOPS;
         hops <= PSP_DNS_STUB_CNAME_HOPS + 1u; hops++) {
        begin_reply(&reply, query, query_length, REPLY_OK, hops + 1u);
        size_t owner = question;
        for (unsigned hop = 0u; hop < hops; hop++) {
            char label[4];
            snprintf(label, sizeof(label), "h%u", hop);
            put_pointer(&reply, owner);
            put_record(&reply, PSP_DNS_STUB_TYPE_CNAME,
                       (unsigned) strlen(label) + 3u);
            owner = put_prefix_then_pointer(&reply, label, example);
        }
        put_pointer(&reply, owner);
        put_a(&reply, 100u, 64u, 0u, 1u);
        PspDnsStubReply result =
            parse(&reply, query, query_length, address);
        CHECK(hops == PSP_DNS_STUB_CNAME_HOPS
                  ? result == PSP_DNS_STUB_ANSWER
                  : result == PSP_DNS_STUB_UNUSABLE);
    }
    return 0;
}

static int test_compression_bounds(void)
{
    uint8_t query[PSP_DNS_STUB_QUERY_BYTES];
    size_t query_length = psp_dns_stub_encode_query(
        query, sizeof(query), 0x0101u, "a.example");
    CHECK(query_length != 0u);
    uint8_t address[4] = {0};
    Packet reply;

    /* An owner name that points at itself. */
    begin_reply(&reply, query, query_length, REPLY_OK, 1u);
    size_t self = reply.length;
    put_pointer(&reply, self);
    put_a(&reply, 1u, 2u, 3u, 4u);
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_UNUSABLE);

    /* A forward pointer. */
    begin_reply(&reply, query, query_length, REPLY_OK, 1u);
    put_pointer(&reply, reply.length + 20u);
    put_a(&reply, 1u, 2u, 3u, 4u);
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_UNUSABLE);

    /* Every pointer points backwards, yet the walk cycles: a 20-byte label
       covers a pointer back into its own bytes, and reading forward from
       there reaches the pointer again. The jump and length bounds end it. */
    Packet cycle = { .length = 0u };
    for (unsigned byte = 0u; byte < 40u; byte++) put8(&cycle, 0u);
    cycle.bytes[10] = 20u;           /* label covering 11..30 */
    cycle.bytes[31] = 0xC0u;         /* pointer at 31 -> 11 */
    cycle.bytes[32] = 11u;
    cycle.bytes[11] = 19u;           /* at 11: label covering 12..30 */
    PspDnsStubNameCursor cursor = { cycle.bytes, cycle.length, 10u, 0u, 0u };
    int labels = 0;
    for (;;) {
        const uint8_t *label = NULL;
        int result = psp_dns_stub_next_label(&cursor, &label);
        if (result <= 0) {
            CHECK(result < 0);
            break;
        }
        CHECK(++labels < 64);
    }
    CHECK(!psp_dns_stub_names_equal(cycle.bytes, cycle.length, 10u,
                                    cycle.bytes, cycle.length, 10u));

    /* Each bound on its own: a forward pointer to a well-formed name is
       refused even though following it would terminate... */
    uint8_t forward[] = { 0xC0u, 2u, 1u, 'a', 0u };
    PspDnsStubNameCursor ahead = { forward, sizeof(forward), 0u, 0u, 0u };
    const uint8_t *forward_label = NULL;
    CHECK(psp_dns_stub_next_label(&ahead, &forward_label) < 0);
    /* ...and a chain of strictly backward pointers ends at the jump
       bound: 16 jumps reach the root, 17 are refused. */
    for (unsigned jumps = PSP_DNS_STUB_POINTER_JUMPS;
         jumps <= PSP_DNS_STUB_POINTER_JUMPS + 1u; jumps++) {
        uint8_t chain[1u + 2u * (PSP_DNS_STUB_POINTER_JUMPS + 1u)];
        chain[0] = 0u; /* root */
        for (unsigned link = 0u; link < jumps; link++) {
            size_t at = 1u + 2u * link;
            size_t previous = link == 0u ? 0u : at - 2u;
            chain[at] = 0xC0u;
            chain[at + 1u] = (uint8_t) previous;
        }
        PspDnsStubNameCursor walk = {
            chain, 1u + 2u * jumps, 1u + 2u * (jumps - 1u), 0u, 0u
        };
        const uint8_t *walk_label = NULL;
        int result = psp_dns_stub_next_label(&walk, &walk_label);
        CHECK(jumps == PSP_DNS_STUB_POINTER_JUMPS ? result == 0
                                                   : result < 0);
    }

    /* A pointer whose second byte is missing, and a reserved label type. */
    uint8_t short_pointer[] = { 0xC0u };
    CHECK(psp_dns_stub_skip_name(short_pointer, 1u, 0u) == 0u);
    uint8_t reserved[] = { 0x40u, 0u };
    CHECK(psp_dns_stub_skip_name(reserved, 2u, 0u) == 0u);
    PspDnsStubNameCursor reserved_cursor = { reserved, 2u, 0u, 0u, 0u };
    const uint8_t *label = NULL;
    CHECK(psp_dns_stub_next_label(&reserved_cursor, &label) < 0);
    return 0;
}

static int test_rejections(void)
{
    uint8_t query[PSP_DNS_STUB_QUERY_BYTES];
    size_t query_length = psp_dns_stub_encode_query(
        query, sizeof(query), 0xA55Au, "video.example.org");
    CHECK(query_length != 0u);
    size_t question = PSP_DNS_STUB_HEADER_BYTES;
    uint8_t address[4] = {0};
    Packet good;
    begin_reply(&good, query, query_length, REPLY_OK, 1u);
    put_pointer(&good, question);
    put_a(&good, 9u, 8u, 7u, 6u);
    CHECK(parse(&good, query, query_length, address)
          == PSP_DNS_STUB_ANSWER);

    /* Wrong ID: someone else's reply, or a forgery. */
    Packet reply = good;
    reply.bytes[1] ^= 0x01u;
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_IGNORE);
    /* Our own query reflected back (QR clear). */
    reply = good;
    reply.bytes[2] &= 0x7Fu;
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_IGNORE);
    /* Not a standard query. */
    reply = good;
    reply.bytes[2] |= 0x10u;
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_IGNORE);
    /* A different question, type, class, or question count. */
    reply = good;
    reply.bytes[question + 1u] = 'x';
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_IGNORE);
    reply = good;
    reply.bytes[query_length - 3u] = 28u;
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_IGNORE);
    reply = good;
    reply.bytes[query_length - 1u] = 3u;
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_IGNORE);
    reply = good;
    reply.bytes[5] = 0u;
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_IGNORE);

    /* NXDOMAIN is definitive, even with records present. */
    reply = good;
    reply.bytes[3] = 0x83u;
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_NXDOMAIN);
    /* SERVFAIL and REFUSED only retire that server. */
    reply = good;
    reply.bytes[3] = 0x82u;
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_UNUSABLE);
    reply = good;
    reply.bytes[3] = 0x85u;
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_UNUSABLE);
    /* TC wins over whatever partial records came with it. */
    reply = good;
    reply.bytes[2] |= 0x02u;
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_TRUNCATED);

    /* An empty NOERROR answer (NODATA). */
    begin_reply(&reply, query, query_length, REPLY_OK, 0u);
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_UNUSABLE);

    /* ANCOUNT promising more records than the packet holds: an address
       found first still answers; otherwise the reply is unusable. */
    reply = good;
    reply.bytes[7] = 5u;
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_ANSWER);
    begin_reply(&reply, query, query_length, REPLY_OK, 3u);
    put_name(&reply, "other.example.org");
    put_a(&reply, 1u, 1u, 1u, 1u);
    CHECK(parse(&reply, query, query_length, address)
          == PSP_DNS_STUB_UNUSABLE);

    /* Every truncation of a good reply is either ignored or unusable, never
       an answer read past the end. */
    for (size_t length = 0u; length < good.length; length++) {
        PspDnsStubReply result = psp_dns_stub_parse_response(
            good.bytes, length, query, query_length, address);
        CHECK(result == PSP_DNS_STUB_IGNORE
              || result == PSP_DNS_STUB_UNUSABLE);
    }
    CHECK(psp_dns_stub_parse_response(
              NULL, 0u, query, query_length, address)
          == PSP_DNS_STUB_IGNORE);

    /* Garbage carrying the right ID and flags: the parser must stay within
       the datagram and terminate, whatever it concludes. */
    uint32_t state = 0x12345678u;
    for (unsigned round = 0u; round < 20000u; round++) {
        Packet noise;
        noise.length = 12u + (round % 500u);
        for (size_t at = 0u; at < noise.length; at++) {
            state = state * 1664525u + 1013904223u;
            noise.bytes[at] = (uint8_t) (state >> 24);
        }
        /* Keep the header plausible so parsing gets past it; half the time
           keep the real question too so the record walk is exercised. */
        noise.bytes[0] = query[0];
        noise.bytes[1] = query[1];
        noise.bytes[2] = 0x81u;
        noise.bytes[3] = 0x80u;
        noise.bytes[4] = 0u;
        noise.bytes[5] = 1u;
        if ((round & 1u) != 0u && noise.length >= query_length)
            memcpy(noise.bytes + 12u, query + 12u, query_length - 12u);
        PspDnsStubReply result = parse(&noise, query, query_length, address);
        CHECK(result >= PSP_DNS_STUB_IGNORE
              && result <= PSP_DNS_STUB_UNUSABLE);
    }
    return 0;
}

static int test_schedule(void)
{
    /* Two servers alternate, primary first, with the second round
       doubled. */
    static const unsigned two_servers[][2] = {
        { 0u, 400u }, { 1u, 400u }, { 0u, 800u }, { 1u, 800u }
    };
    uint32_t total = 0u;
    unsigned server = 99u;
    uint32_t wait_ms = 0u;
    unsigned step = 0u;
    for (; psp_dns_stub_schedule(step, 2u, &server, &wait_ms); step++) {
        CHECK(step < 4u);
        CHECK(server == two_servers[step][0]
              && wait_ms == two_servers[step][1]);
        total += wait_ms;
    }
    CHECK(step == 4u);
    CHECK(total == PSP_DNS_STUB_BUDGET_MS);

    total = 0u;
    for (step = 0u; psp_dns_stub_schedule(step, 1u, &server, &wait_ms);
         step++) {
        CHECK(server == 0u);
        total += wait_ms;
    }
    CHECK(step == 3u && total == PSP_DNS_STUB_BUDGET_MS);
    CHECK(!psp_dns_stub_schedule(0u, 0u, &server, &wait_ms));
    CHECK(!psp_dns_stub_schedule(0u, 2u, NULL, &wait_ms));

    /* The stub never costs more than one stopped firmware attempt, and the
       first wait is well above the slowest healthy device lookup (61 ms). */
    CHECK(PSP_DNS_STUB_BUDGET_MS <= 2500u);
    CHECK(PSP_DNS_STUB_BUDGET_MS < PSP_NETWORK_DNS_HARD_DEADLINE_MS);
    CHECK(psp_dns_stub_schedule(0u, 2u, &server, &wait_ms)
          && wait_ms >= 4u * 61u);

    /* A server that answered unusably is skipped; with both gone the stub
       ends. */
    step = 0u;
    CHECK(psp_dns_stub_next_send(&step, 2u, 1u << 0, &server, &wait_ms)
          && server == 1u && wait_ms == 400u && step == 2u);
    CHECK(psp_dns_stub_next_send(&step, 2u, 1u << 0, &server, &wait_ms)
          && server == 1u && wait_ms == 800u && step == 4u);
    CHECK(!psp_dns_stub_next_send(&step, 2u, 1u << 0, &server, &wait_ms));
    step = 0u;
    CHECK(!psp_dns_stub_next_send(&step, 2u, 3u, &server, &wait_ms));
    step = 0u;
    unsigned sends = 0u;
    while (psp_dns_stub_next_send(&step, 2u, 0u, &server, &wait_ms))
        sends++;
    CHECK(sends == 4u);
    return 0;
}

static int test_policy(void)
{
    CHECK(psp_dns_stub_owns_name("www.example.com"));
    CHECK(psp_dns_stub_owns_name(
        "rr1---sn-jxou0gtapo3-nx5e.googlevideo.com"));
    CHECK(psp_dns_stub_owns_name("example.com."));
    CHECK(psp_dns_stub_owns_name("localhost.example.com"));
    CHECK(!psp_dns_stub_owns_name("localhost"));
    CHECK(!psp_dns_stub_owns_name("localhost."));
    CHECK(!psp_dns_stub_owns_name("intranet"));
    CHECK(!psp_dns_stub_owns_name("app.localhost"));
    CHECK(!psp_dns_stub_owns_name("App.LocalHost."));
    CHECK(!psp_dns_stub_owns_name(".localhost"));
    CHECK(!psp_dns_stub_owns_name(""));
    CHECK(!psp_dns_stub_owns_name(NULL));

    /* Stand-down: only a silent stub followed by a firmware success is a
       strike, a heard reply clears the run, a total failure is neutral. */
    unsigned run = 0u;
    run = psp_dns_stub_silence_after(run, false, true);
    run = psp_dns_stub_silence_after(run, false, false);
    run = psp_dns_stub_silence_after(run, false, true);
    CHECK(run == 2u && !psp_dns_stub_stood_down(run));
    CHECK(psp_dns_stub_silence_after(run, true, true) == 0u);
    run = psp_dns_stub_silence_after(run, false, true);
    CHECK(psp_dns_stub_stood_down(run));
    CHECK(psp_dns_stub_silence_after(run, false, true)
          == PSP_DNS_STUB_SILENT_LIMIT);
    return 0;
}

int main(void)
{
    if (test_encoding() != 0 || test_answers() != 0
        || test_cname_chains() != 0 || test_compression_bounds() != 0
        || test_rejections() != 0 || test_schedule() != 0
        || test_policy() != 0) return 1;
    puts("psp dns stub tests passed");
    return 0;
}
