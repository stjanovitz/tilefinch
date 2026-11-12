/*
 * RFC 6455 payload checks shared by the PSP WebSocket worker
 * (src/fetch/background_websocket.inc), the host security tests and the
 * host loopback test of the worker lane. No transport state lives here.
 */
#include "tilefinch/fetch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

bool fetch_websocket_text_payload_valid(
    const unsigned char *value, size_t length)
{
    if (length != 0 && value == NULL) return false;
    size_t at = 0;
    while (at < length) {
        unsigned char first = value[at++];
        if (first <= 0x7fu) continue;
        if (first >= 0xc2u && first <= 0xdfu) {
            if (at >= length || (value[at++] & 0xc0u) != 0x80u)
                return false;
            continue;
        }
        if (first >= 0xe0u && first <= 0xefu) {
            if (length - at < 2u) return false;
            unsigned char second = value[at++];
            unsigned char third = value[at++];
            if ((third & 0xc0u) != 0x80u
                || (first == 0xe0u
                    ? second < 0xa0u || second > 0xbfu
                    : first == 0xedu
                        ? second < 0x80u || second > 0x9fu
                        : (second & 0xc0u) != 0x80u)) return false;
            continue;
        }
        if (first >= 0xf0u && first <= 0xf4u) {
            if (length - at < 3u) return false;
            unsigned char second = value[at++];
            unsigned char third = value[at++];
            unsigned char fourth = value[at++];
            if ((third & 0xc0u) != 0x80u || (fourth & 0xc0u) != 0x80u
                || (first == 0xf0u
                    ? second < 0x90u || second > 0xbfu
                    : first == 0xf4u
                        ? second < 0x80u || second > 0x8fu
                        : (second & 0xc0u) != 0x80u)) return false;
            continue;
        }
        return false;
    }
    return true;
}

static bool fetch_websocket_peer_close_code_valid(uint16_t code)
{
    if (code >= 3000u && code <= 4999u) return true;
    return code >= 1000u && code <= 1014u
        && code != 1004u && code != 1005u && code != 1006u;
}

FetchWebSocketClosePayloadStatus fetch_websocket_close_payload_parse(
    const unsigned char *payload, size_t length,
    FetchWebSocketClosePayload *parsed)
{
    if (parsed == NULL) return FETCH_WEBSOCKET_CLOSE_PAYLOAD_PROTOCOL_ERROR;
    memset(parsed, 0, sizeof(*parsed));
    parsed->code = 1005u;
    if (length > 125u || (length != 0 && payload == NULL) || length == 1u)
        return FETCH_WEBSOCKET_CLOSE_PAYLOAD_PROTOCOL_ERROR;
    if (length == 0) return FETCH_WEBSOCKET_CLOSE_PAYLOAD_VALID;
    uint16_t code = (uint16_t) (((uint16_t) payload[0] << 8u)
                                | (uint16_t) payload[1]);
    if (!fetch_websocket_peer_close_code_valid(code))
        return FETCH_WEBSOCKET_CLOSE_PAYLOAD_PROTOCOL_ERROR;
    size_t reason_length = length - 2u;
    if (!fetch_websocket_text_payload_valid(payload + 2u, reason_length))
        return FETCH_WEBSOCKET_CLOSE_PAYLOAD_INVALID_UTF8;
    parsed->code = code;
    parsed->has_code = true;
    parsed->reason_length = reason_length;
    if (reason_length != 0)
        memcpy(parsed->reason, payload + 2u, reason_length);
    parsed->reason[reason_length] = '\0';
    return FETCH_WEBSOCKET_CLOSE_PAYLOAD_VALID;
}

FetchWebSocketClosePayloadStatus fetch_websocket_close_payload_accumulate(
    FetchWebSocketCloseAccumulator *accumulator,
    const unsigned char *chunk, size_t chunk_length,
    size_t bytes_remaining, FetchWebSocketClosePayload *parsed)
{
    if (accumulator == NULL || parsed == NULL
        || (chunk_length != 0 && chunk == NULL)) {
        return FETCH_WEBSOCKET_CLOSE_PAYLOAD_PROTOCOL_ERROR;
    }
    if (!accumulator->active) {
        accumulator->active = true;
        accumulator->length = 0;
    }
    if (chunk_length > sizeof(accumulator->bytes) - accumulator->length
        || bytes_remaining > sizeof(accumulator->bytes)
                                  - accumulator->length - chunk_length) {
        accumulator->active = false;
        accumulator->length = 0;
        return FETCH_WEBSOCKET_CLOSE_PAYLOAD_PROTOCOL_ERROR;
    }
    if (chunk_length != 0) {
        memcpy(accumulator->bytes + accumulator->length,
               chunk, chunk_length);
        accumulator->length += chunk_length;
    }
    if (bytes_remaining != 0)
        return FETCH_WEBSOCKET_CLOSE_PAYLOAD_INCOMPLETE;
    FetchWebSocketClosePayloadStatus status =
        fetch_websocket_close_payload_parse(
            accumulator->bytes, accumulator->length, parsed);
    accumulator->active = false;
    accumulator->length = 0;
    return status;
}
