#include "tilefinch/budget.h"
#include "tilefinch/multiplayer.h"
#include "tilefinch/multiplayer_protocol.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        failures++;                                                          \
    }                                                                        \
} while (0)

typedef struct {
    uint64_t next_id;
    bool open;
    bool cancelled;
    bool sent_binary;
    size_t sent_length;
    TilefinchMultiplayerEvent event;
    bool event_pending;
} FakeBackend;

static uint64_t fake_open(void *opaque, Budget *budget,
                          const TilefinchMultiplayerRequest *request)
{
    FakeBackend *fake = opaque;
    if (budget == NULL || request == NULL || fake->open) return 0;
    fake->open = true;
    return ++fake->next_id;
}

static bool fake_send(void *opaque, uint64_t id,
                      const unsigned char *data, size_t length, bool binary)
{
    FakeBackend *fake = opaque;
    if (!fake->open || id != fake->next_id || (length != 0 && data == NULL))
        return false;
    fake->sent_length = length;
    fake->sent_binary = binary;
    return true;
}

static bool fake_accept(void *opaque, uint64_t id,
                        uint32_t peer, bool accepted)
{
    FakeBackend *fake = opaque;
    return fake->open && id == fake->next_id && peer == 7u && accepted;
}

static bool fake_add_code(void *opaque, uint64_t id, const char *code)
{
    FakeBackend *fake = opaque;
    return fake->open && id == fake->next_id && code != NULL;
}

static bool fake_close(void *opaque, uint64_t id,
                       uint16_t code, const char *reason)
{
    FakeBackend *fake = opaque;
    if (!fake->open || id != fake->next_id || code != 1000u
        || strcmp(reason, "done") != 0) return false;
    fake->event = (TilefinchMultiplayerEvent) {
        .kind = TILEFINCH_MULTIPLAYER_EVENT_CLOSE,
        .close_code = code,
        .clean = true
    };
    snprintf(fake->event.detail, sizeof(fake->event.detail), "%s", reason);
    fake->event_pending = true;
    return true;
}

static bool fake_cancel(void *opaque, uint64_t id)
{
    FakeBackend *fake = opaque;
    if (!fake->open || id != fake->next_id) return false;
    fake->open = false;
    fake->cancelled = true;
    return true;
}

static bool fake_take(void *opaque, uint64_t id,
                      unsigned char *data, size_t capacity,
                      TilefinchMultiplayerEvent *event)
{
    FakeBackend *fake = opaque;
    (void) data;
    (void) capacity;
    if (!fake->open || id != fake->next_id || !fake->event_pending)
        return false;
    *event = fake->event;
    fake->event_pending = false;
    if (event->kind == TILEFINCH_MULTIPLAYER_EVENT_CLOSE) fake->open = false;
    return true;
}

static void test_invite_codes(void)
{
    char preferred[TILEFINCH_MULTIPLAYER_INVITE_CODE_LIMIT + 1u];
    char fallback[TILEFINCH_MULTIPLAYER_INVITE_CODE_LIMIT + 1u];
    uint32_t address = UINT32_C(0xcb00710a);
    uint32_t decoded_address = 0;
    uint16_t decoded_port = 0;
    CHECK(tilefinch_multiplayer_invite_encode(
        address, TILEFINCH_MULTIPLAYER_PREFERRED_PORT,
        preferred, sizeof(preferred)));
    CHECK(strlen(preferred) == 12u);
    CHECK(tilefinch_multiplayer_invite_decode(
        preferred, &decoded_address, &decoded_port));
    CHECK(decoded_address == address);
    CHECK(decoded_port == TILEFINCH_MULTIPLAYER_PREFERRED_PORT);
    CHECK(tilefinch_multiplayer_invite_encode(
        address, 49152u, fallback, sizeof(fallback)));
    CHECK(strlen(fallback) == 17u);
    CHECK(tilefinch_multiplayer_invite_decode(
        fallback, &decoded_address, &decoded_port));
    CHECK(decoded_address == address && decoded_port == 49152u);
    fallback[5] = fallback[5] == '9' ? '8' : (char) (fallback[5] + 1);
    CHECK(!tilefinch_multiplayer_invite_decode(
        fallback, &decoded_address, &decoded_port));
    CHECK(!tilefinch_multiplayer_invite_decode(
        "1234-abcd", &decoded_address, &decoded_port));
}

static void test_registry(void)
{
    FakeBackend fake = {0};
    const TilefinchMultiplayerBackend backend = {
        .open = fake_open,
        .send = fake_send,
        .accept = fake_accept,
        .add_remote_code = fake_add_code,
        .close = fake_close,
        .cancel = fake_cancel,
        .take_event = fake_take
    };
    Budget budget;
    budget_init(&budget, 64u * 1024u);
    CHECK(tilefinch_multiplayer_install_backend(&backend, &fake));
    TilefinchMultiplayerRequest request = {
        .mode = TILEFINCH_MULTIPLAYER_MODE_HOST
    };
    snprintf(request.game_id, sizeof(request.game_id), "test-game");
    snprintf(request.peer_name, sizeof(request.peer_name), "Bird");
    uint64_t id = tilefinch_multiplayer_open(&budget, &request);
    CHECK(id == 1u);
    CHECK(tilefinch_multiplayer_open(&budget, &request) == 0u);
    const unsigned char packet[] = {1u, 2u, 3u};
    CHECK(tilefinch_multiplayer_send(id, packet, sizeof(packet), true));
    CHECK(fake.sent_binary && fake.sent_length == sizeof(packet));
    CHECK(!tilefinch_multiplayer_send(
        id, packet, TILEFINCH_MULTIPLAYER_MESSAGE_LIMIT + 1u, true));
    CHECK(tilefinch_multiplayer_accept(id, 7u, true));
    CHECK(tilefinch_multiplayer_close(id, 1000u, "done"));
    TilefinchMultiplayerEvent event = {0};
    unsigned char scratch[TILEFINCH_MULTIPLAYER_MESSAGE_LIMIT];
    CHECK(tilefinch_multiplayer_take_event(
        id, scratch, sizeof(scratch), &event));
    CHECK(event.kind == TILEFINCH_MULTIPLAYER_EVENT_CLOSE && event.clean);
    CHECK(tilefinch_multiplayer_open(&budget, &request) == 2u);
    CHECK(tilefinch_multiplayer_cancel(2u));
    CHECK(fake.cancelled);
    CHECK(budget.current == 0u);
}

static void test_wire_protocol(void)
{
    unsigned char payload[TILEFINCH_MULTIPLAYER_MESSAGE_LIMIT];
    unsigned char packet[TILEFINCH_MULTIPLAYER_WIRE_PACKET_LIMIT];
    for (size_t at = 0; at < sizeof(payload); at++)
        payload[at] = (unsigned char) (at ^ (at >> 3));
    size_t length = tilefinch_multiplayer_packet_encode(
        TILEFINCH_MULTIPLAYER_PACKET_DATA, UINT32_C(0x12345678),
        UINT32_C(0x89abcdef), UINT32_C(0x10203040),
        UINT32_C(0x55667788), UINT16_C(0xfffe), true,
        payload, sizeof(payload), packet, sizeof(packet));
    CHECK(length == sizeof(packet));
    TilefinchMultiplayerWirePacket decoded = {0};
    CHECK(tilefinch_multiplayer_packet_decode(packet, length, &decoded));
    CHECK(decoded.kind == TILEFINCH_MULTIPLAYER_PACKET_DATA
          && decoded.game_hash == UINT32_C(0x12345678)
          && decoded.sender == UINT32_C(0x89abcdef)
          && decoded.receiver == UINT32_C(0x10203040)
          && decoded.cookie == UINT32_C(0x55667788)
          && decoded.sequence == UINT16_C(0xfffe)
          && decoded.binary && decoded.payload_length == sizeof(payload)
          && memcmp(decoded.payload, payload, sizeof(payload)) == 0);
    CHECK(!tilefinch_multiplayer_packet_decode(packet, length - 1u, &decoded));
    CHECK(!tilefinch_multiplayer_packet_encode(
        TILEFINCH_MULTIPLAYER_PACKET_DATA, 1u, 2u, 3u, 4u, 5u,
        false, payload, sizeof(payload), packet, sizeof(packet) - 1u));
    packet[4]++;
    CHECK(!tilefinch_multiplayer_packet_decode(packet, length, &decoded));
    packet[4]--;
    packet[7] = 1u;
    CHECK(!tilefinch_multiplayer_packet_decode(packet, length, &decoded));
    packet[7] = 0;
    packet[5] = 0xffu;
    CHECK(!tilefinch_multiplayer_packet_decode(packet, length, &decoded));
}

int main(void)
{
    test_invite_codes();
    test_wire_protocol();
    test_registry();
    if (failures != 0) {
        fprintf(stderr, "%d multiplayer checks failed\n", failures);
        return 1;
    }
    puts("multiplayer tests passed");
    return 0;
}
