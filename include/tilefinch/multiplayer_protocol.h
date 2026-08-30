#ifndef TILEFINCH_MULTIPLAYER_PROTOCOL_H
#define TILEFINCH_MULTIPLAYER_PROTOCOL_H

#include "tilefinch/multiplayer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TILEFINCH_MULTIPLAYER_WIRE_HEADER_BYTES 28u
#define TILEFINCH_MULTIPLAYER_WIRE_PACKET_LIMIT \
    (TILEFINCH_MULTIPLAYER_WIRE_HEADER_BYTES \
     + TILEFINCH_MULTIPLAYER_MESSAGE_LIMIT)

typedef enum {
    TILEFINCH_MULTIPLAYER_PACKET_ADVERTISE = 1,
    TILEFINCH_MULTIPLAYER_PACKET_PUNCH,
    TILEFINCH_MULTIPLAYER_PACKET_HELLO,
    TILEFINCH_MULTIPLAYER_PACKET_APPROVE,
    TILEFINCH_MULTIPLAYER_PACKET_CONFIRM,
    TILEFINCH_MULTIPLAYER_PACKET_OPEN,
    TILEFINCH_MULTIPLAYER_PACKET_DATA,
    TILEFINCH_MULTIPLAYER_PACKET_PING,
    TILEFINCH_MULTIPLAYER_PACKET_PONG,
    TILEFINCH_MULTIPLAYER_PACKET_CLOSE
} TilefinchMultiplayerPacketKind;

typedef struct {
    TilefinchMultiplayerPacketKind kind;
    uint32_t game_hash;
    uint32_t sender;
    uint32_t receiver;
    uint32_t cookie;
    uint16_t sequence;
    bool binary;
    const unsigned char *payload;
    size_t payload_length;
} TilefinchMultiplayerWirePacket;

size_t tilefinch_multiplayer_packet_encode(
    TilefinchMultiplayerPacketKind kind, uint32_t game_hash,
    uint32_t sender, uint32_t receiver, uint32_t cookie,
    uint16_t sequence, bool binary, const unsigned char *payload,
    size_t payload_length, unsigned char *output, size_t capacity);

bool tilefinch_multiplayer_packet_decode(
    const unsigned char *bytes, size_t length,
    TilefinchMultiplayerWirePacket *packet);

#endif
