#include "tilefinch/multiplayer_protocol.h"

#include <string.h>

#define TILEFINCH_MULTIPLAYER_WIRE_MAGIC UINT32_C(0x54464d50)
#define TILEFINCH_MULTIPLAYER_WIRE_VERSION 1u

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

static bool multiplayer_packet_kind_valid(unsigned value)
{
    return value >= TILEFINCH_MULTIPLAYER_PACKET_ADVERTISE
        && value <= TILEFINCH_MULTIPLAYER_PACKET_CLOSE;
}

size_t tilefinch_multiplayer_packet_encode(
    TilefinchMultiplayerPacketKind kind, uint32_t game_hash,
    uint32_t sender, uint32_t receiver, uint32_t cookie,
    uint16_t sequence, bool binary, const unsigned char *payload,
    size_t payload_length, unsigned char *output, size_t capacity)
{
    if (!multiplayer_packet_kind_valid((unsigned) kind)
        || game_hash == 0 || sender == 0 || output == NULL
        || payload_length > TILEFINCH_MULTIPLAYER_MESSAGE_LIMIT
        || (payload_length != 0 && payload == NULL)
        || capacity < TILEFINCH_MULTIPLAYER_WIRE_HEADER_BYTES
        || payload_length
               > capacity - TILEFINCH_MULTIPLAYER_WIRE_HEADER_BYTES)
        return 0;
    multiplayer_write_u32(output, TILEFINCH_MULTIPLAYER_WIRE_MAGIC);
    output[4] = TILEFINCH_MULTIPLAYER_WIRE_VERSION;
    output[5] = (unsigned char) kind;
    output[6] = binary ? 1u : 0u;
    output[7] = 0;
    multiplayer_write_u32(output + 8, game_hash);
    multiplayer_write_u32(output + 12, sender);
    multiplayer_write_u32(output + 16, receiver);
    multiplayer_write_u16(output + 20, sequence);
    multiplayer_write_u16(output + 22, (uint16_t) payload_length);
    multiplayer_write_u32(output + 24, cookie);
    if (payload_length != 0)
        memcpy(output + TILEFINCH_MULTIPLAYER_WIRE_HEADER_BYTES,
               payload, payload_length);
    return TILEFINCH_MULTIPLAYER_WIRE_HEADER_BYTES + payload_length;
}

bool tilefinch_multiplayer_packet_decode(
    const unsigned char *bytes, size_t length,
    TilefinchMultiplayerWirePacket *packet)
{
    if (bytes == NULL || packet == NULL
        || length < TILEFINCH_MULTIPLAYER_WIRE_HEADER_BYTES
        || length > TILEFINCH_MULTIPLAYER_WIRE_PACKET_LIMIT
        || multiplayer_read_u32(bytes) != TILEFINCH_MULTIPLAYER_WIRE_MAGIC
        || bytes[4] != TILEFINCH_MULTIPLAYER_WIRE_VERSION
        || !multiplayer_packet_kind_valid(bytes[5])
        || (bytes[6] & ~1u) != 0 || bytes[7] != 0)
        return false;
    size_t payload_length = multiplayer_read_u16(bytes + 22);
    if (payload_length != length - TILEFINCH_MULTIPLAYER_WIRE_HEADER_BYTES
        || payload_length > TILEFINCH_MULTIPLAYER_MESSAGE_LIMIT)
        return false;
    uint32_t game_hash = multiplayer_read_u32(bytes + 8);
    uint32_t sender = multiplayer_read_u32(bytes + 12);
    if (game_hash == 0 || sender == 0) return false;
    *packet = (TilefinchMultiplayerWirePacket) {
        .kind = (TilefinchMultiplayerPacketKind) bytes[5],
        .game_hash = game_hash,
        .sender = sender,
        .receiver = multiplayer_read_u32(bytes + 16),
        .cookie = multiplayer_read_u32(bytes + 24),
        .sequence = multiplayer_read_u16(bytes + 20),
        .binary = (bytes[6] & 1u) != 0,
        .payload = bytes + TILEFINCH_MULTIPLAYER_WIRE_HEADER_BYTES,
        .payload_length = payload_length
    };
    return true;
}
