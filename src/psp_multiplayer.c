#include "psp_multiplayer.h"

#include "tilefinch/multiplayer.h"
#include "tilefinch/multiplayer_protocol.h"
#include "tilefinch/platform.h"
#include "tilefinch/psp_threads.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pspkernel.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <pspthreadman.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#define PSP_MULTIPLAYER_THREAD_STACK (16u * 1024u)
#define PSP_MULTIPLAYER_EVENT_STORAGE 10u
#define PSP_MULTIPLAYER_COMMAND_STORAGE 8u
#define PSP_MULTIPLAYER_PACKET_LIMIT TILEFINCH_MULTIPLAYER_WIRE_PACKET_LIMIT
#define PSP_MULTIPLAYER_STUN_BYTES 20u
#define PSP_MULTIPLAYER_STUN_SERVER "stun.cloudflare.com"
#define PSP_MULTIPLAYER_STUN_PORT 3478u
#define PSP_MULTIPLAYER_STUN_COOKIE UINT32_C(0x2112a442)

typedef enum {
    PSP_MULTIPLAYER_COMMAND_SEND = 1,
    PSP_MULTIPLAYER_COMMAND_ACCEPT,
    PSP_MULTIPLAYER_COMMAND_REMOTE_CODE,
    PSP_MULTIPLAYER_COMMAND_CLOSE
} PspMultiplayerCommandKind;

typedef struct {
    PspMultiplayerCommandKind kind;
    size_t length;
    uint32_t peer_identity;
    uint16_t close_code;
    bool accepted;
    bool binary;
    char code[TILEFINCH_MULTIPLAYER_INVITE_CODE_LIMIT + 1u];
    char reason[64];
    unsigned char data[TILEFINCH_MULTIPLAYER_MESSAGE_LIMIT];
} PspMultiplayerCommand;

typedef struct {
    TilefinchMultiplayerEvent event;
    unsigned char data[TILEFINCH_MULTIPLAYER_MESSAGE_LIMIT];
} PspMultiplayerQueuedEvent;

typedef struct {
    Budget *budget;
    BudgetReservation stack_reservation;
    SceUID thread;
    atomic_uint command_read;
    atomic_uint command_write;
    atomic_uint event_read;
    atomic_uint event_write;
    atomic_int stop_requested;
    atomic_int worker_done;
    PspMultiplayerCommand commands[PSP_MULTIPLAYER_COMMAND_STORAGE];
    PspMultiplayerQueuedEvent events[PSP_MULTIPLAYER_EVENT_STORAGE];
    TilefinchMultiplayerRequest request;
    uint64_t id;
    int socket;
    uint32_t game_hash;
    uint32_t local_nonce;
    uint32_t remote_nonce;
    uint32_t candidate_nonce;
    uint32_t cookie;
    uint32_t cookie_secret;
    uint16_t sequence;
    uint16_t bound_port;
    struct sockaddr_in remote;
    struct sockaddr_in candidate;
    struct sockaddr_in punch_endpoint;
    bool remote_valid;
    bool candidate_valid;
    bool candidate_reported;
    bool candidate_accepted;
    bool punch_valid;
    bool opened;
    bool close_queued;
    unsigned char stun_transaction[12];
    bool stun_pending;
    bool stun_complete;
    bool stun_fallback_reported;
    unsigned stun_attempts;
    uint64_t started_us;
    uint64_t created_us;
    uint64_t last_advertise_us;
    uint64_t last_handshake_us;
    uint64_t last_receive_us;
    uint64_t last_stun_us;
    uint64_t last_punch_us;
    char candidate_name[TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT + 1u];
} PspMultiplayerSession;

_Static_assert(sizeof(PspMultiplayerSession) < 24u * 1024u,
               "multiplayer session must remain a small bounded page owner");

typedef struct {
    PspProcessResources *process;
    PspBrowserResources *browser;
    PspNetwork *network;
    PspNetworkLifecycle *network_lifecycle;
    PspMultiplayerSession *session;
    uint32_t next_id;
    bool network_requested;
} PspMultiplayerContext;

static PspMultiplayerContext psp_multiplayer;

static uint16_t multiplayer_read_u16(const unsigned char *bytes)
{
    return (uint16_t) (((uint16_t) bytes[0] << 8) | bytes[1]);
}

static uint32_t multiplayer_read_u32(const unsigned char *bytes)
{
    return ((uint32_t) bytes[0] << 24) | ((uint32_t) bytes[1] << 16)
        | ((uint32_t) bytes[2] << 8) | bytes[3];
}

static void multiplayer_write_u16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char) (value >> 8);
    bytes[1] = (unsigned char) value;
}

static void multiplayer_write_u32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char) (value >> 24);
    bytes[1] = (unsigned char) (value >> 16);
    bytes[2] = (unsigned char) (value >> 8);
    bytes[3] = (unsigned char) value;
}

static uint32_t multiplayer_hash(const char *text)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t at = 0; text != NULL && text[at] != '\0'; at++) {
        hash ^= (unsigned char) text[at];
        hash *= UINT32_C(16777619);
    }
    return hash == 0 ? 1u : hash;
}

static bool multiplayer_same_endpoint(const struct sockaddr_in *left,
                                      const struct sockaddr_in *right)
{
    return left != NULL && right != NULL
        && left->sin_family == AF_INET && right->sin_family == AF_INET
        && left->sin_port == right->sin_port
        && left->sin_addr.s_addr == right->sin_addr.s_addr;
}

static bool multiplayer_push_event(PspMultiplayerSession *session,
                                   const TilefinchMultiplayerEvent *event,
                                   const unsigned char *data)
{
    unsigned write = atomic_load_explicit(
        &session->event_write, memory_order_relaxed);
    unsigned read = atomic_load_explicit(
        &session->event_read, memory_order_acquire);
    unsigned used = write - read;
    bool terminal = event->kind == TILEFINCH_MULTIPLAYER_EVENT_CLOSE;
    bool essential = event->kind == TILEFINCH_MULTIPLAYER_EVENT_OPEN
        || event->kind == TILEFINCH_MULTIPLAYER_EVENT_DRAIN;
    if (used >= PSP_MULTIPLAYER_EVENT_STORAGE
        || (!terminal && !essential
            && used >= PSP_MULTIPLAYER_EVENT_STORAGE - 2u)
        || (!terminal && essential
            && used >= PSP_MULTIPLAYER_EVENT_STORAGE - 1u))
        return false;
    PspMultiplayerQueuedEvent *queued =
        &session->events[write % PSP_MULTIPLAYER_EVENT_STORAGE];
    queued->event = *event;
    if (event->length != 0 && data != NULL)
        memcpy(queued->data, data, event->length);
    atomic_store_explicit(
        &session->event_write, write + 1u, memory_order_release);
    return true;
}

static void multiplayer_status(PspMultiplayerSession *session,
                               const char *detail)
{
    TilefinchMultiplayerEvent event = {
        .kind = TILEFINCH_MULTIPLAYER_EVENT_STATUS
    };
    snprintf(event.detail, sizeof(event.detail), "%s", detail);
    (void) multiplayer_push_event(session, &event, NULL);
}

static void multiplayer_close_event(PspMultiplayerSession *session,
                                    uint16_t code, bool clean,
                                    const char *detail)
{
    if (session->close_queued) return;
    TilefinchMultiplayerEvent event = {
        .kind = TILEFINCH_MULTIPLAYER_EVENT_CLOSE,
        .close_code = code,
        .clean = clean
    };
    snprintf(event.detail, sizeof(event.detail), "%s", detail);
    if (multiplayer_push_event(session, &event, NULL))
        session->close_queued = true;
}

static bool multiplayer_send_packet(
    PspMultiplayerSession *session, const struct sockaddr_in *destination,
    TilefinchMultiplayerPacketKind kind, bool binary,
    const unsigned char *payload, size_t length)
{
    unsigned char packet[PSP_MULTIPLAYER_PACKET_LIMIT];
    size_t bytes = tilefinch_multiplayer_packet_encode(
        kind, session->game_hash, session->local_nonce,
        session->remote_nonce, session->cookie, ++session->sequence,
        binary, payload, length, packet, sizeof(packet));
    if (bytes == 0 || destination == NULL) return false;
    int sent = sceNetInetSendto(
        session->socket, packet, bytes, 0,
        (const struct sockaddr *) destination, sizeof(*destination));
    return sent >= 0 && (size_t) sent == bytes;
}

static bool multiplayer_decode_endpoint(const char *code,
                                        struct sockaddr_in *endpoint)
{
    uint32_t address = 0;
    uint16_t port = 0;
    if (!tilefinch_multiplayer_invite_decode(code, &address, &port))
        return false;
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->sin_family = AF_INET;
    endpoint->sin_port = htons(port);
    endpoint->sin_addr.s_addr = htonl(address);
    return true;
}

static void multiplayer_emit_invite(PspMultiplayerSession *session,
                                    uint32_t address, uint16_t port,
                                    const char *detail)
{
    TilefinchMultiplayerEvent event = {
        .kind = TILEFINCH_MULTIPLAYER_EVENT_INVITE_CODE
    };
    if (!tilefinch_multiplayer_invite_encode(
            address, port, event.invite_code, sizeof(event.invite_code)))
        return;
    snprintf(event.detail, sizeof(event.detail), "%s", detail);
    (void) multiplayer_push_event(session, &event, NULL);
}

static bool multiplayer_stun_begin(PspMultiplayerSession *session)
{
    session->stun_attempts++;
    session->last_stun_us = tilefinch_platform_monotonic_time_us();
    session->stun_pending = false;
    unsigned char resolver_storage[1024];
    int resolver = -1;
    struct in_addr address;
    if (sceNetResolverCreate(
            &resolver, resolver_storage, sizeof(resolver_storage)) < 0)
        return false;
    int resolved = sceNetResolverStartNtoA(
        resolver, PSP_MULTIPLAYER_STUN_SERVER, &address, 2u, 1);
    (void) sceNetResolverDelete(resolver);
    if (resolved < 0) return false;
    unsigned char request[PSP_MULTIPLAYER_STUN_BYTES] = {0};
    multiplayer_write_u16(request, 0x0001u);
    multiplayer_write_u16(request + 2, 0u);
    multiplayer_write_u32(request + 4, PSP_MULTIPLAYER_STUN_COOKIE);
    memcpy(request + 8, session->stun_transaction,
           sizeof(session->stun_transaction));
    struct sockaddr_in endpoint = {0};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(PSP_MULTIPLAYER_STUN_PORT);
    endpoint.sin_addr = address;
    int sent = sceNetInetSendto(
        session->socket, request, sizeof(request), 0,
        (const struct sockaddr *) &endpoint, sizeof(endpoint));
    session->stun_pending = sent >= 0 && (size_t) sent == sizeof(request);
    return session->stun_pending;
}

static bool multiplayer_stun_response(
    PspMultiplayerSession *session, const unsigned char *packet, size_t length)
{
    if (!session->stun_pending || length < PSP_MULTIPLAYER_STUN_BYTES
        || multiplayer_read_u16(packet) != 0x0101u
        || multiplayer_read_u32(packet + 4) != PSP_MULTIPLAYER_STUN_COOKIE
        || memcmp(packet + 8, session->stun_transaction,
                  sizeof(session->stun_transaction)) != 0) return false;
    size_t body = multiplayer_read_u16(packet + 2);
    if (body > length - PSP_MULTIPLAYER_STUN_BYTES) return false;
    size_t at = PSP_MULTIPLAYER_STUN_BYTES;
    size_t end = at + body;
    while (at <= end && end - at >= 4u) {
        uint16_t type = multiplayer_read_u16(packet + at);
        size_t bytes = multiplayer_read_u16(packet + at + 2u);
        at += 4u;
        if (bytes > end - at) return false;
        if (type == 0x0020u && bytes >= 8u && packet[at + 1u] == 0x01u) {
            uint16_t port = multiplayer_read_u16(packet + at + 2u)
                ^ (uint16_t) (PSP_MULTIPLAYER_STUN_COOKIE >> 16);
            uint32_t address = multiplayer_read_u32(packet + at + 4u)
                ^ PSP_MULTIPLAYER_STUN_COOKIE;
            if (port != 0 && address != 0) {
                session->stun_pending = false;
                session->stun_complete = true;
                multiplayer_emit_invite(session, address, port, "internet");
                return true;
            }
        }
        size_t padded = (bytes + 3u) & ~(size_t) 3u;
        if (padded > end - at) return false;
        at += padded;
    }
    return false;
}

static void multiplayer_emit_peer(PspMultiplayerSession *session,
                                  TilefinchMultiplayerEventKind kind,
                                  const struct sockaddr_in *source,
                                  uint32_t peer, const char *name,
                                  const char *detail)
{
    TilefinchMultiplayerEvent event = {
        .kind = kind,
        .peer_identity = peer
    };
    snprintf(event.peer_name, sizeof(event.peer_name), "%s", name);
    snprintf(event.detail, sizeof(event.detail), "%s", detail);
    (void) tilefinch_multiplayer_invite_encode(
        ntohl(source->sin_addr.s_addr), ntohs(source->sin_port),
        event.invite_code, sizeof(event.invite_code));
    (void) multiplayer_push_event(session, &event, NULL);
}

static void multiplayer_mark_open(PspMultiplayerSession *session)
{
    if (session->opened) return;
    session->opened = true;
    session->last_receive_us = tilefinch_platform_monotonic_time_us();
    TilefinchMultiplayerEvent event = {
        .kind = TILEFINCH_MULTIPLAYER_EVENT_OPEN,
        .peer_identity = session->remote_nonce
    };
    snprintf(event.peer_name, sizeof(event.peer_name), "%s",
             session->candidate_name);
    snprintf(event.detail, sizeof(event.detail), "direct-udp");
    (void) multiplayer_push_event(session, &event, NULL);
}

static void multiplayer_receive(PspMultiplayerSession *session,
                                const unsigned char *packet, size_t length,
                                const struct sockaddr_in *source)
{
    if (multiplayer_stun_response(session, packet, length)) return;
    TilefinchMultiplayerWirePacket decoded = {0};
    if (!tilefinch_multiplayer_packet_decode(packet, length, &decoded)
        || decoded.game_hash != session->game_hash) return;
    TilefinchMultiplayerPacketKind kind = decoded.kind;
    bool binary = decoded.binary;
    uint32_t sender = decoded.sender;
    uint32_t receiver = decoded.receiver;
    size_t payload_length = decoded.payload_length;
    uint32_t cookie = decoded.cookie;
    const unsigned char *payload = decoded.payload;
    if (kind == TILEFINCH_MULTIPLAYER_PACKET_ADVERTISE
        && session->request.mode == TILEFINCH_MULTIPLAYER_MODE_DISCOVER) {
        char name[TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT + 1u] = {0};
        size_t copy = payload_length < TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT
            ? payload_length : TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT;
        memcpy(name, payload, copy);
        multiplayer_emit_peer(
            session, TILEFINCH_MULTIPLAYER_EVENT_DISCOVERED,
            source, sender, name, "lan");
        return;
    }
    if (kind == TILEFINCH_MULTIPLAYER_PACKET_PUNCH && !session->opened) {
        if (session->request.mode == TILEFINCH_MULTIPLAYER_MODE_JOIN
            || session->request.mode == TILEFINCH_MULTIPLAYER_MODE_DISCOVER) {
            session->request.mode = TILEFINCH_MULTIPLAYER_MODE_JOIN;
            session->remote = *source;
            session->remote_valid = true;
            session->remote_nonce = 0;
            session->cookie = 0;
            session->last_handshake_us = 0;
        }
        return;
    }
    if (kind == TILEFINCH_MULTIPLAYER_PACKET_HELLO
        && session->request.mode == TILEFINCH_MULTIPLAYER_MODE_HOST
        && !session->opened) {
        if (!session->candidate_valid || sender != session->candidate_nonce
            || !multiplayer_same_endpoint(source, &session->candidate)) {
            session->candidate = *source;
            session->candidate_nonce = sender;
            session->candidate_valid = true;
            session->candidate_reported = false;
            session->candidate_accepted = false;
            size_t copy = payload_length < TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT
                ? payload_length : TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT;
            memcpy(session->candidate_name, payload, copy);
            session->candidate_name[copy] = '\0';
        }
        if (!session->candidate_reported) {
            multiplayer_emit_peer(
                session, TILEFINCH_MULTIPLAYER_EVENT_PEER_REQUEST,
                source, sender, session->candidate_name, "incoming");
            session->candidate_reported = true;
        }
        return;
    }
    if (kind == TILEFINCH_MULTIPLAYER_PACKET_APPROVE
        && session->request.mode != TILEFINCH_MULTIPLAYER_MODE_HOST
        && receiver == session->local_nonce && cookie != 0) {
        session->remote = *source;
        session->remote_valid = true;
        session->remote_nonce = sender;
        session->cookie = cookie;
        size_t copy = payload_length < TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT
            ? payload_length : TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT;
        memcpy(session->candidate_name, payload, copy);
        session->candidate_name[copy] = '\0';
        (void) multiplayer_send_packet(
            session, &session->remote, TILEFINCH_MULTIPLAYER_PACKET_CONFIRM,
            false, NULL, 0);
        session->last_handshake_us = 0;
        return;
    }
    if (kind == TILEFINCH_MULTIPLAYER_PACKET_CONFIRM
        && session->request.mode == TILEFINCH_MULTIPLAYER_MODE_HOST
        && session->candidate_accepted && session->remote_valid
        && multiplayer_same_endpoint(source, &session->remote)
        && sender == session->remote_nonce
        && receiver == session->local_nonce && cookie == session->cookie) {
        multiplayer_mark_open(session);
        (void) multiplayer_send_packet(
            session, &session->remote, TILEFINCH_MULTIPLAYER_PACKET_OPEN,
            false, NULL, 0);
        return;
    }
    if (!session->remote_valid
        || !multiplayer_same_endpoint(source, &session->remote)
        || sender != session->remote_nonce
        || receiver != session->local_nonce || cookie != session->cookie)
        return;
    session->last_receive_us = tilefinch_platform_monotonic_time_us();
    if (kind == TILEFINCH_MULTIPLAYER_PACKET_OPEN) {
        multiplayer_mark_open(session);
    } else if (kind == TILEFINCH_MULTIPLAYER_PACKET_DATA && session->opened) {
        TilefinchMultiplayerEvent event = {
            .kind = TILEFINCH_MULTIPLAYER_EVENT_MESSAGE,
            .length = payload_length,
            .binary = binary,
            .peer_identity = sender
        };
        (void) multiplayer_push_event(session, &event, payload);
    } else if (kind == TILEFINCH_MULTIPLAYER_PACKET_PING) {
        (void) multiplayer_send_packet(
            session, &session->remote, TILEFINCH_MULTIPLAYER_PACKET_PONG,
            false, NULL, 0);
    } else if (kind == TILEFINCH_MULTIPLAYER_PACKET_CLOSE) {
        multiplayer_close_event(session, 1000u, true, "peer closed");
        atomic_store_explicit(
            &session->stop_requested, 1, memory_order_release);
    }
}

static void multiplayer_apply_command(PspMultiplayerSession *session,
                                      const PspMultiplayerCommand *command)
{
    if (command->kind == PSP_MULTIPLAYER_COMMAND_SEND) {
        bool sent = session->opened && session->remote_valid
            && multiplayer_send_packet(
                   session, &session->remote, TILEFINCH_MULTIPLAYER_PACKET_DATA,
                   command->binary, command->data, command->length);
        if (sent) {
            TilefinchMultiplayerEvent event = {
                .kind = TILEFINCH_MULTIPLAYER_EVENT_DRAIN,
                .sent_bytes = command->length
            };
            (void) multiplayer_push_event(session, &event, NULL);
        } else {
            multiplayer_close_event(session, 1006u, false, "send failed");
            atomic_store_explicit(
                &session->stop_requested, 1, memory_order_release);
        }
    } else if (command->kind == PSP_MULTIPLAYER_COMMAND_ACCEPT) {
        if (session->candidate_valid
            && command->peer_identity == session->candidate_nonce) {
            if (!command->accepted) {
                session->candidate_valid = false;
                session->candidate_reported = false;
            } else {
                uint32_t cookie = session->cookie_secret
                    ^ session->local_nonce ^ session->candidate_nonce
                    ^ ntohl(session->candidate.sin_addr.s_addr)
                    ^ (uint32_t) ntohs(session->candidate.sin_port);
                session->cookie = cookie == 0 ? 1u : cookie;
                session->remote = session->candidate;
                session->remote_valid = true;
                session->remote_nonce = session->candidate_nonce;
                session->candidate_accepted = true;
                session->last_handshake_us = 0;
            }
        }
    } else if (command->kind == PSP_MULTIPLAYER_COMMAND_REMOTE_CODE) {
        struct sockaddr_in endpoint;
        if (multiplayer_decode_endpoint(command->code, &endpoint)) {
            if (session->request.mode == TILEFINCH_MULTIPLAYER_MODE_HOST) {
                session->punch_endpoint = endpoint;
                session->punch_valid = true;
                session->last_punch_us = 0;
            } else {
                session->remote = endpoint;
                session->remote_valid = true;
                session->remote_nonce = 0;
                session->cookie = 0;
                session->request.mode = TILEFINCH_MULTIPLAYER_MODE_JOIN;
                session->last_handshake_us = 0;
            }
            multiplayer_status(session, "punching");
        }
    } else if (command->kind == PSP_MULTIPLAYER_COMMAND_CLOSE) {
        if (session->opened && session->remote_valid)
            (void) multiplayer_send_packet(
                session, &session->remote, TILEFINCH_MULTIPLAYER_PACKET_CLOSE,
                false, (const unsigned char *) command->reason,
                strlen(command->reason));
        multiplayer_close_event(
            session, command->close_code, true, command->reason);
        atomic_store_explicit(
            &session->stop_requested, 1, memory_order_release);
    }
}

static void multiplayer_service_commands(PspMultiplayerSession *session)
{
    unsigned read = atomic_load_explicit(
        &session->command_read, memory_order_relaxed);
    unsigned write = atomic_load_explicit(
        &session->command_write, memory_order_acquire);
    for (unsigned count = 0; read != write && count < 4u; count++, read++)
        multiplayer_apply_command(
            session,
            &session->commands[read % PSP_MULTIPLAYER_COMMAND_STORAGE]);
    atomic_store_explicit(
        &session->command_read, read, memory_order_release);
}

static void multiplayer_periodic(PspMultiplayerSession *session,
                                 uint64_t now_us)
{
    if (session->request.mode == TILEFINCH_MULTIPLAYER_MODE_HOST
        && !session->opened
        && now_us - session->last_advertise_us >= UINT64_C(1000000)) {
        struct sockaddr_in broadcast = {0};
        broadcast.sin_family = AF_INET;
        broadcast.sin_port = htons(TILEFINCH_MULTIPLAYER_PREFERRED_PORT);
        broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        (void) multiplayer_send_packet(
            session, &broadcast, TILEFINCH_MULTIPLAYER_PACKET_ADVERTISE,
            false, (const unsigned char *) session->request.peer_name,
            strlen(session->request.peer_name));
        session->last_advertise_us = now_us;
    }
    if (session->punch_valid && !session->opened
        && now_us - session->last_punch_us >= UINT64_C(250000)) {
        (void) multiplayer_send_packet(
            session, &session->punch_endpoint,
            TILEFINCH_MULTIPLAYER_PACKET_PUNCH, false,
            (const unsigned char *) session->request.peer_name,
            strlen(session->request.peer_name));
        session->last_punch_us = now_us;
    }
    if (session->request.mode == TILEFINCH_MULTIPLAYER_MODE_JOIN
        && session->remote_valid && !session->opened
        && session->remote_nonce == 0
        && now_us - session->last_handshake_us >= UINT64_C(250000)) {
        (void) multiplayer_send_packet(
            session, &session->remote, TILEFINCH_MULTIPLAYER_PACKET_HELLO,
            false, (const unsigned char *) session->request.peer_name,
            strlen(session->request.peer_name));
        session->last_handshake_us = now_us;
    } else if (session->request.mode == TILEFINCH_MULTIPLAYER_MODE_JOIN
               && session->remote_valid && !session->opened
               && session->remote_nonce != 0
               && now_us - session->last_handshake_us
                      >= UINT64_C(250000)) {
        (void) multiplayer_send_packet(
            session, &session->remote, TILEFINCH_MULTIPLAYER_PACKET_CONFIRM,
            false, NULL, 0);
        session->last_handshake_us = now_us;
    } else if (session->request.mode == TILEFINCH_MULTIPLAYER_MODE_HOST
               && session->candidate_accepted && !session->opened
               && now_us - session->last_handshake_us
                      >= UINT64_C(250000)) {
        (void) multiplayer_send_packet(
            session, &session->remote, TILEFINCH_MULTIPLAYER_PACKET_APPROVE,
            false, (const unsigned char *) session->request.peer_name,
            strlen(session->request.peer_name));
        session->last_handshake_us = now_us;
    }
    if (session->opened
        && now_us - session->last_handshake_us >= UINT64_C(1000000)) {
        (void) multiplayer_send_packet(
            session, &session->remote, TILEFINCH_MULTIPLAYER_PACKET_PING,
            false, NULL, 0);
        session->last_handshake_us = now_us;
    }
    if (!session->opened && now_us - session->started_us > UINT64_C(30000000)) {
        multiplayer_close_event(session, 1006u, false, "connection timed out");
        atomic_store_explicit(
            &session->stop_requested, 1, memory_order_release);
    } else if (session->opened && session->last_receive_us != 0
               && now_us - session->last_receive_us > UINT64_C(15000000)) {
        multiplayer_close_event(session, 1006u, false, "peer timed out");
        atomic_store_explicit(
            &session->stop_requested, 1, memory_order_release);
    }
}

static int multiplayer_worker(SceSize arguments, void *argument)
{
    PspMultiplayerSession *session = NULL;
    if (argument != NULL && arguments == sizeof(session))
        memcpy(&session, argument, sizeof(session));
    if (session == NULL) return -1;
    session->socket = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
    if (session->socket < 0) {
        multiplayer_close_event(session, 1006u, false, "UDP unavailable");
        atomic_store_explicit(&session->worker_done, 1, memory_order_release);
        return 0;
    }
    int enabled = 1;
    (void) sceNetInetSetsockopt(
        session->socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    (void) sceNetInetSetsockopt(
        session->socket, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled));
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(TILEFINCH_MULTIPLAYER_PREFERRED_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (sceNetInetBind(
            session->socket, (const struct sockaddr *) &local,
            sizeof(local)) < 0) {
        local.sin_port = 0;
        if (sceNetInetBind(
                session->socket, (const struct sockaddr *) &local,
                sizeof(local)) < 0) {
            multiplayer_close_event(session, 1006u, false, "UDP bind failed");
            goto done;
        }
    }
    socklen_t local_length = sizeof(local);
    if (sceNetInetGetsockname(
            session->socket, (struct sockaddr *) &local,
            &local_length) == 0)
        session->bound_port = ntohs(local.sin_port);
    multiplayer_status(session, "finding route");
    (void) multiplayer_stun_begin(session);
    (void) sceKernelChangeThreadPriority(
        0, TILEFINCH_PSP_THREAD_PRIORITY_TRANSPORT);
    session->started_us = tilefinch_platform_monotonic_time_us();
    if (session->request.mode == TILEFINCH_MULTIPLAYER_MODE_JOIN)
        session->remote_valid = multiplayer_decode_endpoint(
            session->request.invite_code, &session->remote);
    while (!atomic_load_explicit(
               &session->stop_requested, memory_order_acquire)) {
        multiplayer_service_commands(session);
        struct SceNetInetPollfd descriptor = {
            .fd = session->socket,
            .events = SCE_NET_INET_POLLIN
        };
        int ready = sceNetInetPoll(&descriptor, 1u, 10);
        for (unsigned received = 0;
             ready > 0 && (descriptor.revents & SCE_NET_INET_POLLIN) != 0
                 && received < 4u;
             received++) {
            unsigned char packet[PSP_MULTIPLAYER_PACKET_LIMIT];
            struct sockaddr_in source = {0};
            socklen_t source_length = sizeof(source);
            int received_bytes = sceNetInetRecvfrom(
                session->socket, packet, sizeof(packet), 0,
                (struct sockaddr *) &source, &source_length);
            if (received_bytes <= 0) break;
            multiplayer_receive(
                session, packet, (size_t) received_bytes, &source);
            descriptor.revents = 0;
            ready = sceNetInetPoll(&descriptor, 1u, 0);
        }
        uint64_t now_us = tilefinch_platform_monotonic_time_us();
        multiplayer_periodic(session, now_us);
        if (!session->stun_complete && session->stun_pending
            && now_us - session->last_stun_us > UINT64_C(1500000))
            session->stun_pending = false;
        if (!session->stun_complete && !session->stun_pending
            && session->stun_attempts < 3u
            && now_us - session->last_stun_us >= UINT64_C(250000))
            (void) multiplayer_stun_begin(session);
        if (!session->stun_complete && !session->stun_pending
            && session->stun_attempts >= 3u
            && !session->stun_fallback_reported) {
            multiplayer_status(session, "LAN or two-code fallback");
            session->stun_fallback_reported = true;
        }
    }
done:
    if (session->socket >= 0) {
        (void) sceNetInetClose(session->socket);
        session->socket = -1;
    }
    atomic_store_explicit(&session->worker_done, 1, memory_order_release);
    return 0;
}

static bool multiplayer_enqueue(PspMultiplayerSession *session,
                                const PspMultiplayerCommand *command)
{
    if (session == NULL || command == NULL) return false;
    unsigned write = atomic_load_explicit(
        &session->command_write, memory_order_relaxed);
    unsigned read = atomic_load_explicit(
        &session->command_read, memory_order_acquire);
    if (write - read >= PSP_MULTIPLAYER_COMMAND_STORAGE) return false;
    session->commands[write % PSP_MULTIPLAYER_COMMAND_STORAGE] = *command;
    atomic_store_explicit(
        &session->command_write, write + 1u, memory_order_release);
    return true;
}

static uint64_t multiplayer_backend_open(
    void *opaque, Budget *budget, const TilefinchMultiplayerRequest *request)
{
    PspMultiplayerContext *context = opaque;
    if (context == NULL || context->process == NULL
        || context->network == NULL || context->network_lifecycle == NULL
        || context->session != NULL)
        return 0;
    PspMultiplayerSession *session = budget_calloc_category(
        budget, BUDGET_CATEGORY_JAVASCRIPT, 1u, sizeof(*session));
    if (session == NULL) return 0;
    session->budget = budget;
    session->thread = -1;
    session->socket = -1;
    session->request = *request;
    session->created_us = tilefinch_platform_monotonic_time_us();
    session->game_hash = multiplayer_hash(request->game_id);
    if (!tilefinch_platform_secure_random(
            &session->local_nonce, sizeof(session->local_nonce))
        || session->local_nonce == 0
        || !tilefinch_platform_secure_random(
               &session->cookie_secret, sizeof(session->cookie_secret))
        || session->cookie_secret == 0
        || !tilefinch_platform_secure_random(
               session->stun_transaction,
               sizeof(session->stun_transaction))
        || !budget_reservation_acquire(
               &session->stack_reservation, budget,
               BUDGET_CATEGORY_RESOURCE, PSP_MULTIPLAYER_THREAD_STACK)) {
        budget_reservation_release(&session->stack_reservation);
        budget_free(budget, session);
        return 0;
    }
    atomic_init(&session->command_read, 0u);
    atomic_init(&session->command_write, 0u);
    atomic_init(&session->event_read, 0u);
    atomic_init(&session->event_write, 0u);
    atomic_init(&session->stop_requested, 0);
    atomic_init(&session->worker_done, 0);
    context->next_id++;
    if (context->next_id == 0) context->next_id++;
    session->id = context->next_id;
    context->session = session;
    context->network_requested = true;
    psp_network_lifecycle_request(
        context->network_lifecycle, PSP_NETWORK_REQUEST_MULTIPLAYER, true,
        (int) context->process->config.network_profile, context->network,
        PSP_NETWORK_SUPERVISOR_STATE_COUNT, "multiplayer-open");
    multiplayer_status(session, "connecting Wi-Fi");
    return session->id;
}

static bool multiplayer_backend_send(
    void *opaque, uint64_t id, const unsigned char *data,
    size_t length, bool binary)
{
    PspMultiplayerContext *context = opaque;
    if (context == NULL || context->session == NULL
        || context->session->id != id || length > TILEFINCH_MULTIPLAYER_MESSAGE_LIMIT)
        return false;
    PspMultiplayerCommand command = {
        .kind = PSP_MULTIPLAYER_COMMAND_SEND,
        .length = length,
        .binary = binary
    };
    if (length != 0) memcpy(command.data, data, length);
    return multiplayer_enqueue(context->session, &command);
}

static bool multiplayer_backend_accept(
    void *opaque, uint64_t id, uint32_t peer, bool accepted)
{
    PspMultiplayerContext *context = opaque;
    if (context == NULL || context->session == NULL
        || context->session->id != id) return false;
    PspMultiplayerCommand command = {
        .kind = PSP_MULTIPLAYER_COMMAND_ACCEPT,
        .peer_identity = peer,
        .accepted = accepted
    };
    return multiplayer_enqueue(context->session, &command);
}

static bool multiplayer_backend_remote_code(
    void *opaque, uint64_t id, const char *code)
{
    PspMultiplayerContext *context = opaque;
    if (context == NULL || context->session == NULL
        || context->session->id != id) return false;
    PspMultiplayerCommand command = {
        .kind = PSP_MULTIPLAYER_COMMAND_REMOTE_CODE
    };
    snprintf(command.code, sizeof(command.code), "%s", code);
    return multiplayer_enqueue(context->session, &command);
}

static bool multiplayer_backend_close(
    void *opaque, uint64_t id, uint16_t code, const char *reason)
{
    PspMultiplayerContext *context = opaque;
    if (context == NULL || context->session == NULL
        || context->session->id != id) return false;
    PspMultiplayerCommand command = {
        .kind = PSP_MULTIPLAYER_COMMAND_CLOSE,
        .close_code = code
    };
    snprintf(command.reason, sizeof(command.reason), "%s", reason);
    return multiplayer_enqueue(context->session, &command);
}

static void multiplayer_release(PspMultiplayerContext *context)
{
    PspMultiplayerSession *session = context == NULL ? NULL : context->session;
    if (session == NULL) return;
    if (session->thread >= 0) {
        (void) sceKernelWaitThreadEnd(session->thread, NULL);
        (void) sceKernelDeleteThread(session->thread);
        session->thread = -1;
    }
    psp_network_lifecycle_request(
        context->network_lifecycle, PSP_NETWORK_REQUEST_MULTIPLAYER,
        false, (int) context->process->config.network_profile,
        context->network, PSP_NETWORK_SUPERVISOR_STATE_COUNT,
        "multiplayer-close");
    Budget *budget = session->budget;
    budget_reservation_release(&session->stack_reservation);
    context->session = NULL;
    context->network_requested = false;
    budget_free(budget, session);
}

static bool multiplayer_backend_cancel(void *opaque, uint64_t id)
{
    PspMultiplayerContext *context = opaque;
    if (context == NULL || context->session == NULL
        || context->session->id != id) return false;
    atomic_store_explicit(
        &context->session->stop_requested, 1, memory_order_release);
    multiplayer_release(context);
    return true;
}

static bool multiplayer_backend_take(
    void *opaque, uint64_t id, unsigned char *data, size_t capacity,
    TilefinchMultiplayerEvent *event)
{
    PspMultiplayerContext *context = opaque;
    PspMultiplayerSession *session =
        context == NULL ? NULL : context->session;
    if (session == NULL || session->id != id || event == NULL) return false;
    unsigned read = atomic_load_explicit(
        &session->event_read, memory_order_relaxed);
    unsigned write = atomic_load_explicit(
        &session->event_write, memory_order_acquire);
    if (read == write) return false;
    PspMultiplayerQueuedEvent *queued =
        &session->events[read % PSP_MULTIPLAYER_EVENT_STORAGE];
    if (queued->event.length > capacity
        || (queued->event.length != 0 && data == NULL)) return false;
    *event = queued->event;
    if (event->length != 0) memcpy(data, queued->data, event->length);
    atomic_store_explicit(
        &session->event_read, read + 1u, memory_order_release);
    if (event->kind == TILEFINCH_MULTIPLAYER_EVENT_CLOSE)
        multiplayer_release(context);
    return true;
}

bool psp_multiplayer_bind(
    PspProcessResources *process, PspBrowserResources *browser,
    PspNetwork *network, PspNetworkLifecycle *network_lifecycle)
{
    if (process == NULL || browser == NULL || network == NULL
        || network_lifecycle == NULL || psp_multiplayer.session != NULL)
        return false;
    psp_multiplayer.process = process;
    psp_multiplayer.browser = browser;
    psp_multiplayer.network = network;
    psp_multiplayer.network_lifecycle = network_lifecycle;
    static const TilefinchMultiplayerBackend backend = {
        .open = multiplayer_backend_open,
        .send = multiplayer_backend_send,
        .accept = multiplayer_backend_accept,
        .add_remote_code = multiplayer_backend_remote_code,
        .close = multiplayer_backend_close,
        .cancel = multiplayer_backend_cancel,
        .take_event = multiplayer_backend_take
    };
    return tilefinch_multiplayer_install_backend(&backend, &psp_multiplayer);
}

void psp_multiplayer_pump(PspApp *app)
{
    PspMultiplayerSession *session = psp_multiplayer.session;
    if (app == NULL || app->process != psp_multiplayer.process
        || app->network != psp_multiplayer.network || session == NULL
        || session->thread >= 0) return;
    if (!psp_network_lifecycle_ready(app->network_lifecycle)
        || app->network->status != PSP_NETWORK_READY) {
        uint64_t now_us = tilefinch_platform_monotonic_time_us();
        if (!session->close_queued
            && now_us - session->created_us > UINT64_C(30000000)) {
            multiplayer_close_event(
                session, 1006u, false, "network connection timed out");
            atomic_store_explicit(
                &session->worker_done, 1, memory_order_release);
        }
        return;
    }
    session->thread = sceKernelCreateThread(
        "tilefinch_multiplayer", multiplayer_worker,
        TILEFINCH_PSP_THREAD_PRIORITY_TRANSPORT_SETUP,
        PSP_MULTIPLAYER_THREAD_STACK, PSP_THREAD_ATTR_USER, NULL);
    PspMultiplayerSession *argument = session;
    if (session->thread < 0
        || sceKernelStartThread(
               session->thread, sizeof(argument), &argument) < 0) {
        if (session->thread >= 0) {
            (void) sceKernelDeleteThread(session->thread);
            session->thread = -1;
        }
        multiplayer_close_event(session, 1006u, false, "worker unavailable");
        atomic_store_explicit(&session->worker_done, 1, memory_order_release);
    }
}

void psp_multiplayer_shutdown(PspApp *app)
{
    if (app == NULL || app->process != psp_multiplayer.process) return;
    if (psp_multiplayer.session != NULL) {
        atomic_store_explicit(
            &psp_multiplayer.session->stop_requested, 1,
            memory_order_release);
        multiplayer_release(&psp_multiplayer);
    }
    (void) tilefinch_multiplayer_install_backend(NULL, NULL);
    psp_multiplayer.process = NULL;
    psp_multiplayer.browser = NULL;
    psp_multiplayer.network = NULL;
    psp_multiplayer.network_lifecycle = NULL;
}
