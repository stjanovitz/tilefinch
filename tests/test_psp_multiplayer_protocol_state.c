#include "tilefinch/multiplayer.h"

#include <arpa/inet.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* Only the socket/command carriers and publication effects are substituted.
   The peer transitions and SPSC mailbox are the actual device implementation. */
#define PSP_MULTIPLAYER_COMMAND_STORAGE 8u
typedef enum {
    PSP_MULTIPLAYER_CANDIDATE_NONE, PSP_MULTIPLAYER_CANDIDATE_REPORTED,
    PSP_MULTIPLAYER_CANDIDATE_ACCEPTED
} PspMultiplayerCandidate;
typedef enum {
    PSP_MULTIPLAYER_COMMAND_SEND = 1, PSP_MULTIPLAYER_COMMAND_ACCEPT,
    PSP_MULTIPLAYER_COMMAND_REMOTE_CODE, PSP_MULTIPLAYER_COMMAND_CLOSE
} PspMultiplayerCommandKind;
typedef struct {
    PspMultiplayerCommandKind kind;
    size_t length;
    uint32_t peer_identity;
    uint16_t close_code;
    bool accepted, binary;
    char code[TILEFINCH_MULTIPLAYER_INVITE_CODE_LIMIT + 1u];
    char reason[64];
    unsigned char data[TILEFINCH_MULTIPLAYER_MESSAGE_LIMIT];
} PspMultiplayerCommand;
typedef struct {
    struct sockaddr_in candidate, remote;
    uint32_t candidate_nonce, remote_nonce, local_nonce, cookie, cookie_secret;
    char candidate_name[TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT + 1u];
    bool remote_valid, opened;
    PspMultiplayerCandidate candidate_state;
    uint64_t last_handshake_us;
    atomic_uint command_read, command_write;
    PspMultiplayerCommand commands[PSP_MULTIPLAYER_COMMAND_STORAGE];
} PspMultiplayerSession;

static bool notification_admitted;
static unsigned notifications;
static uint32_t notified_peer;
static bool multiplayer_same_endpoint(const struct sockaddr_in *a,
                                      const struct sockaddr_in *b)
{
    return a->sin_family == AF_INET && b->sin_family == AF_INET
        && a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr;
}
static bool multiplayer_emit_peer(PspMultiplayerSession *session,
    TilefinchMultiplayerEventKind kind, const struct sockaddr_in *source,
    uint32_t peer, const char *name, const char *detail)
{
    (void) session; (void) source; (void) name; (void) detail;
    if (kind != TILEFINCH_MULTIPLAYER_EVENT_PEER_REQUEST) return false;
    notifications++;
    if (!notification_admitted) return false;
    notified_peer = peer;
    return true;
}
#include "../src/psp_multiplayer_peer.inc"

static unsigned processed[32], processed_count;
static bool producer_on_drain, successor_send, successor_close, command_stable;
static unsigned routine_admissions;
static bool multiplayer_enqueue(PspMultiplayerSession *, const PspMultiplayerCommand *);
static void multiplayer_apply_command(PspMultiplayerSession *session,
                                      const PspMultiplayerCommand *command)
{
    unsigned identity = command->peer_identity;
    if (processed_count < 32) processed[processed_count++] = identity;
    if (!producer_on_drain || command->kind != PSP_MULTIPLAYER_COMMAND_SEND) return;
    producer_on_drain = false;
    /* Drain may wake the producer before the consumer returns from this call.
       Refill through wraparound; the retired slot must no longer be borrowed. */
    PspMultiplayerCommand next = {.kind = PSP_MULTIPLAYER_COMMAND_ACCEPT};
    routine_admissions = 0;
    for (unsigned i = 0; i < PSP_MULTIPLAYER_COMMAND_STORAGE; i++) {
        next.peer_identity = 10u + i;
        if (!multiplayer_enqueue(session, &next)) break;
        routine_admissions++;
    }
    next.kind = PSP_MULTIPLAYER_COMMAND_SEND;
    next.peer_identity = 20;
    successor_send = multiplayer_enqueue(session, &next);
    next.kind = PSP_MULTIPLAYER_COMMAND_CLOSE;
    next.peer_identity = 21;
    successor_close = multiplayer_enqueue(session, &next);
    command_stable = command->peer_identity == identity && command->data[0] == 0x5a;
}
#include "../src/psp_multiplayer_commands.inc"

static struct sockaddr_in endpoint(unsigned id)
{
    struct sockaddr_in result = {0};
    result.sin_family = AF_INET;
    result.sin_addr.s_addr = htonl(UINT32_C(0xc0000200) + id);
    result.sin_port = htons(29170);
    return result;
}
static void candidate(PspMultiplayerSession *session, unsigned id)
{
    struct sockaddr_in source = endpoint(id);
    multiplayer_consider_candidate(session, &source, id,
                                  (const unsigned char *) "Peer", 4);
}
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", name, __LINE__, #c); return false; \
} } while (0)

static bool run_case(const char *name)
{
    PspMultiplayerSession session = {.local_nonce = 99, .cookie_secret = 123};
    atomic_init(&session.command_read, 0);
    atomic_init(&session.command_write, 0);
    notifications = notified_peer = processed_count = 0;
    notification_admitted = command_stable = true;
    producer_on_drain = successor_send = successor_close = false;
    struct sockaddr_in a = endpoint(1), b = endpoint(2);
    if (!strcmp(name, "pending-stable")) {
        candidate(&session, 1);
        candidate(&session, 2);
        CHECK(session.candidate_nonce == 1 && notified_peer == 1 && notifications == 1);
        multiplayer_apply_peer_approval(&session, 1, true);
        CHECK(session.candidate_state == PSP_MULTIPLAYER_CANDIDATE_ACCEPTED);
    } else if (!strcmp(name, "approved-stable")) {
        candidate(&session, 1);
        multiplayer_apply_peer_approval(&session, 1, true);
        uint32_t cookie = session.cookie;
        candidate(&session, 2);
        CHECK(multiplayer_candidate_confirms(&session, &a, 1, 99, cookie));
        CHECK(session.candidate_nonce == 1 && notifications == 1);
    } else if (!strcmp(name, "notification-refusal")) {
        notification_admitted = false;
        candidate(&session, 1);
        CHECK(session.candidate_state == PSP_MULTIPLAYER_CANDIDATE_NONE
            && session.candidate_nonce == 0 && !session.remote_valid);
        notification_admitted = true;
        candidate(&session, 1);
        CHECK(session.candidate_state == PSP_MULTIPLAYER_CANDIDATE_REPORTED
            && notified_peer == 1 && notifications == 2);
    } else if (!strcmp(name, "reject-reset")) {
        candidate(&session, 1);
        multiplayer_apply_peer_approval(&session, 1, true);
        multiplayer_apply_peer_approval(&session, 1, false);
        CHECK(session.candidate_state == PSP_MULTIPLAYER_CANDIDATE_NONE
            && !session.remote_valid && session.remote_nonce == 0 && session.cookie == 0);
        candidate(&session, 2);
        CHECK(notified_peer == 2);
    } else if (!strcmp(name, "late-decision")) {
        candidate(&session, 1);
        multiplayer_apply_peer_approval(&session, 1, true);
        uint32_t cookie = session.cookie;
        session.opened = true;
        multiplayer_apply_peer_approval(&session, 1, false);
        CHECK(multiplayer_candidate_confirms(&session, &a, 1, 99, cookie));
        multiplayer_apply_peer_approval(&session, 1, true);
        CHECK(session.cookie == cookie && session.opened);
    } else if (!strcmp(name, "confirmation-controls")) {
        candidate(&session, 1);
        candidate(&session, 1);
        CHECK(notifications == 1);
        multiplayer_apply_peer_approval(&session, 1, false);
        candidate(&session, 2);
        multiplayer_apply_peer_approval(&session, 1, true);
        CHECK(session.candidate_state == PSP_MULTIPLAYER_CANDIDATE_REPORTED);
        multiplayer_apply_peer_approval(&session, 2, true);
        uint32_t cookie = session.cookie;
        CHECK(multiplayer_candidate_confirms(&session, &b, 2, 99, cookie));
        CHECK(!multiplayer_candidate_confirms(&session, &a, 2, 99, cookie));
        CHECK(!multiplayer_candidate_confirms(&session, &b, 1, 99, cookie));
        CHECK(!multiplayer_candidate_confirms(&session, &b, 2, 98, cookie));
        CHECK(!multiplayer_candidate_confirms(&session, &b, 2, 99, cookie ^ 1));
    } else if (!strcmp(name, "essential-admission")) {
        PspMultiplayerCommand command = {.kind = PSP_MULTIPLAYER_COMMAND_ACCEPT};
        unsigned admitted = 0;
        while (admitted < 8 && multiplayer_enqueue(&session, &command)) admitted++;
        CHECK(admitted == 6);
        command.kind = PSP_MULTIPLAYER_COMMAND_SEND;
        CHECK(multiplayer_enqueue(&session, &command));
        command.kind = PSP_MULTIPLAYER_COMMAND_CLOSE;
        CHECK(multiplayer_enqueue(&session, &command));
        CHECK(!multiplayer_enqueue(&session, &command));
    } else if (!strcmp(name, "drain-progress")) {
        PspMultiplayerCommand command = {.kind = PSP_MULTIPLAYER_COMMAND_SEND,
            .peer_identity = 1, .data = {0x5a}};
        CHECK(multiplayer_enqueue(&session, &command));
        producer_on_drain = true;
        multiplayer_service_commands(&session);
        CHECK(processed_count == 1 && routine_admissions == 6
            && successor_send && successor_close && command_stable);
        CHECK(atomic_load(&session.command_write) - atomic_load(&session.command_read) == 8);
        multiplayer_service_commands(&session);
        CHECK(processed_count == 5);
        multiplayer_service_commands(&session);
        CHECK(processed_count == 9 && processed[7] == 20 && processed[8] == 21);
    } else if (!strcmp(name, "fifo-wrap")) {
        atomic_store(&session.command_read, UINT_MAX - 3u);
        atomic_store(&session.command_write, UINT_MAX - 3u);
        PspMultiplayerCommand command = {.kind = PSP_MULTIPLAYER_COMMAND_REMOTE_CODE};
        for (unsigned i = 0; i < 8; i++) {
            command.peer_identity = i;
            if (i == 6) command.kind = PSP_MULTIPLAYER_COMMAND_SEND;
            if (i == 7) command.kind = PSP_MULTIPLAYER_COMMAND_CLOSE;
            CHECK(multiplayer_enqueue(&session, &command));
        }
        multiplayer_service_commands(&session);
        CHECK(processed_count == 4);
        multiplayer_service_commands(&session);
        CHECK(processed_count == 8);
        for (unsigned i = 0; i < 8; i++) CHECK(processed[i] == i);
        CHECK(atomic_load(&session.command_read) == atomic_load(&session.command_write));
    } else return false;
    return true;
}

int main(int argc, char **argv)
{
    const char *cases[] = {"pending-stable", "approved-stable", "notification-refusal",
        "reject-reset", "late-decision", "confirmation-controls", "essential-admission",
        "drain-progress", "fifo-wrap"};
    bool ok = true;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 1 && strcmp(argv[1], cases[i])) continue;
        bool passed = run_case(cases[i]);
        printf("%s: %s\n", cases[i], passed ? "PASS" : "FAIL");
        ok = passed && ok;
    }
    return ok ? 0 : 1;
}
