#include "tilefinch/multiplayer.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const TilefinchMultiplayerBackend *backend;
    void *context;
    atomic_ullong active_session;
} TilefinchMultiplayerRegistry;

static TilefinchMultiplayerRegistry multiplayer_registry;

static size_t multiplayer_bounded_length(const char *text, size_t limit)
{
    if (text == NULL) return 0;
    size_t length = 0;
    while (length <= limit && text[length] != '\0') length++;
    return length;
}

static bool multiplayer_text_valid(const char *text, size_t limit,
                                   bool allow_empty)
{
    if (text == NULL) return allow_empty;
    size_t length = multiplayer_bounded_length(text, limit);
    if (length > limit || (!allow_empty && length == 0)) return false;
    for (size_t at = 0; at < length; at++) {
        unsigned char value = (unsigned char) text[at];
        if (value < 0x20u || value == 0x7fu) return false;
    }
    return true;
}

static uint8_t multiplayer_endpoint_checksum(
    uint32_t address, uint16_t port, bool port_present)
{
    uint32_t value = UINT32_C(0x54464d50) ^ address;
    value ^= (uint32_t) port << 7;
    value ^= port_present ? UINT32_C(0x19d4) : UINT32_C(0x2a71);
    value ^= value >> 16;
    value *= UINT32_C(0x45d9f3b);
    value ^= value >> 16;
    return (uint8_t) (value & 0x3fu);
}

bool tilefinch_multiplayer_install_backend(
    const TilefinchMultiplayerBackend *backend, void *context)
{
    if (atomic_load_explicit(
            &multiplayer_registry.active_session, memory_order_acquire) != 0)
        return false;
    multiplayer_registry.backend = backend;
    multiplayer_registry.context = context;
    return true;
}

uint64_t tilefinch_multiplayer_open(
    Budget *budget, const TilefinchMultiplayerRequest *request)
{
    const TilefinchMultiplayerBackend *backend = multiplayer_registry.backend;
    if (budget == NULL || request == NULL || backend == NULL
        || backend->open == NULL
        || (request->mode != TILEFINCH_MULTIPLAYER_MODE_HOST
            && request->mode != TILEFINCH_MULTIPLAYER_MODE_JOIN
            && request->mode != TILEFINCH_MULTIPLAYER_MODE_DISCOVER)
        || !multiplayer_text_valid(
               request->game_id, TILEFINCH_MULTIPLAYER_GAME_ID_LIMIT, false)
        || !multiplayer_text_valid(
               request->peer_name, TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT, false)
        || !multiplayer_text_valid(
               request->invite_code,
               TILEFINCH_MULTIPLAYER_INVITE_CODE_LIMIT,
               request->mode != TILEFINCH_MULTIPLAYER_MODE_JOIN)
        || atomic_load_explicit(
               &multiplayer_registry.active_session,
               memory_order_acquire) != 0) return 0;
    uint64_t id = backend->open(
        multiplayer_registry.context, budget, request);
    if (id == 0) return 0;
    uint64_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &multiplayer_registry.active_session, &expected, id,
            memory_order_acq_rel, memory_order_acquire)) {
        if (backend->cancel != NULL)
            (void) backend->cancel(multiplayer_registry.context, id);
        return 0;
    }
    return id;
}

static bool multiplayer_session_matches(uint64_t session_id)
{
    return session_id != 0
        && atomic_load_explicit(
               &multiplayer_registry.active_session, memory_order_acquire)
               == session_id;
}

bool tilefinch_multiplayer_send(
    uint64_t session_id, const unsigned char *data, size_t length,
    bool binary)
{
    const TilefinchMultiplayerBackend *backend = multiplayer_registry.backend;
    return multiplayer_session_matches(session_id)
        && backend != NULL && backend->send != NULL
        && length <= TILEFINCH_MULTIPLAYER_MESSAGE_LIMIT
        && (length == 0 || data != NULL)
        && backend->send(
               multiplayer_registry.context, session_id, data, length, binary);
}

bool tilefinch_multiplayer_accept(
    uint64_t session_id, uint32_t peer_identity, bool accepted)
{
    const TilefinchMultiplayerBackend *backend = multiplayer_registry.backend;
    return multiplayer_session_matches(session_id)
        && peer_identity != 0 && backend != NULL && backend->accept != NULL
        && backend->accept(multiplayer_registry.context, session_id,
                           peer_identity, accepted);
}

bool tilefinch_multiplayer_add_remote_code(
    uint64_t session_id, const char *invite_code)
{
    const TilefinchMultiplayerBackend *backend = multiplayer_registry.backend;
    uint32_t address = 0;
    uint16_t port = 0;
    return multiplayer_session_matches(session_id)
        && tilefinch_multiplayer_invite_decode(
               invite_code, &address, &port)
        && address != 0 && port != 0
        && backend != NULL && backend->add_remote_code != NULL
        && backend->add_remote_code(
               multiplayer_registry.context, session_id, invite_code);
}

bool tilefinch_multiplayer_close(
    uint64_t session_id, uint16_t code, const char *reason)
{
    const TilefinchMultiplayerBackend *backend = multiplayer_registry.backend;
    return multiplayer_session_matches(session_id)
        && multiplayer_text_valid(reason, 63u, true)
        && backend != NULL && backend->close != NULL
        && backend->close(
               multiplayer_registry.context, session_id, code,
               reason == NULL ? "" : reason);
}

bool tilefinch_multiplayer_cancel(uint64_t session_id)
{
    const TilefinchMultiplayerBackend *backend = multiplayer_registry.backend;
    if (!multiplayer_session_matches(session_id) || backend == NULL
        || backend->cancel == NULL) return false;
    bool cancelled = backend->cancel(multiplayer_registry.context, session_id);
    if (cancelled) atomic_store_explicit(
        &multiplayer_registry.active_session, 0, memory_order_release);
    return cancelled;
}

bool tilefinch_multiplayer_take_event(
    uint64_t session_id, unsigned char *data, size_t capacity,
    TilefinchMultiplayerEvent *event)
{
    const TilefinchMultiplayerBackend *backend = multiplayer_registry.backend;
    if (!multiplayer_session_matches(session_id) || backend == NULL
        || backend->take_event == NULL || event == NULL) return false;
    bool taken = backend->take_event(
        multiplayer_registry.context, session_id, data, capacity, event);
    if (taken && event->kind == TILEFINCH_MULTIPLAYER_EVENT_CLOSE)
        atomic_store_explicit(
            &multiplayer_registry.active_session, 0, memory_order_release);
    return taken;
}

bool tilefinch_multiplayer_invite_encode(
    uint32_t address, uint16_t port, char *output, size_t output_size)
{
    if (address == 0 || port == 0 || output == NULL || output_size == 0)
        return false;
    bool preferred = port == TILEFINCH_MULTIPLAYER_PREFERRED_PORT;
    uint64_t value = ((uint64_t) address << 6)
        | multiplayer_endpoint_checksum(address, port, !preferred);
    int width = 12;
    if (!preferred) {
        value = ((uint64_t) address << 22)
            | ((uint64_t) port << 6)
            | multiplayer_endpoint_checksum(address, port, true);
        width = 17;
    }
    int written = snprintf(output, output_size, "%0*llu", width,
                           (unsigned long long) value);
    return written == width && (size_t) written < output_size;
}

bool tilefinch_multiplayer_invite_decode(
    const char *input, uint32_t *address, uint16_t *port)
{
    if (input == NULL || address == NULL || port == NULL) return false;
    uint64_t value = 0;
    size_t digits = 0;
    for (size_t at = 0; input[at] != '\0'; at++) {
        unsigned char byte = (unsigned char) input[at];
        if (byte == ' ' || byte == '-') continue;
        if (!isdigit(byte) || digits >= 17u
            || value > (UINT64_MAX - (uint64_t) (byte - '0')) / 10u)
            return false;
        value = value * 10u + (uint64_t) (byte - '0');
        digits++;
    }
    if (digits != 12u && digits != 17u) return false;
    uint8_t checksum = (uint8_t) (value & 0x3fu);
    uint16_t decoded_port = TILEFINCH_MULTIPLAYER_PREFERRED_PORT;
    uint32_t decoded_address = 0;
    if (digits == 12u) {
        decoded_address = (uint32_t) (value >> 6);
        if ((value >> 6) > UINT32_MAX) return false;
    } else {
        decoded_port = (uint16_t) ((value >> 6) & 0xffffu);
        decoded_address = (uint32_t) (value >> 22);
        if ((value >> 22) > UINT32_MAX) return false;
    }
    if (decoded_address == 0 || decoded_port == 0
        || checksum != multiplayer_endpoint_checksum(
               decoded_address, decoded_port, digits == 17u)) return false;
    *address = decoded_address;
    *port = decoded_port;
    return true;
}
