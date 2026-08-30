#ifndef TILEFINCH_MULTIPLAYER_H
#define TILEFINCH_MULTIPLAYER_H

#include "tilefinch/budget.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TILEFINCH_MULTIPLAYER_GAME_ID_LIMIT 32u
#define TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT 24u
#define TILEFINCH_MULTIPLAYER_INVITE_CODE_LIMIT 18u
#define TILEFINCH_MULTIPLAYER_MESSAGE_LIMIT 512u
#define TILEFINCH_MULTIPLAYER_QUEUE_LIMIT 8u
#define TILEFINCH_MULTIPLAYER_DISCOVERY_LIMIT 6u
#define TILEFINCH_MULTIPLAYER_PREFERRED_PORT 29170u

typedef enum {
    TILEFINCH_MULTIPLAYER_MODE_HOST = 1,
    TILEFINCH_MULTIPLAYER_MODE_JOIN = 2,
    TILEFINCH_MULTIPLAYER_MODE_DISCOVER = 3
} TilefinchMultiplayerMode;

typedef enum {
    TILEFINCH_MULTIPLAYER_EVENT_STATUS = 1,
    TILEFINCH_MULTIPLAYER_EVENT_INVITE_CODE,
    TILEFINCH_MULTIPLAYER_EVENT_DISCOVERED,
    TILEFINCH_MULTIPLAYER_EVENT_PEER_REQUEST,
    TILEFINCH_MULTIPLAYER_EVENT_OPEN,
    TILEFINCH_MULTIPLAYER_EVENT_MESSAGE,
    TILEFINCH_MULTIPLAYER_EVENT_DRAIN,
    TILEFINCH_MULTIPLAYER_EVENT_CLOSE
} TilefinchMultiplayerEventKind;

typedef struct {
    TilefinchMultiplayerMode mode;
    char game_id[TILEFINCH_MULTIPLAYER_GAME_ID_LIMIT + 1u];
    char peer_name[TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT + 1u];
    char invite_code[TILEFINCH_MULTIPLAYER_INVITE_CODE_LIMIT + 1u];
} TilefinchMultiplayerRequest;

typedef struct {
    TilefinchMultiplayerEventKind kind;
    size_t length;
    size_t sent_bytes;
    uint32_t peer_identity;
    uint16_t close_code;
    bool clean;
    bool binary;
    char invite_code[TILEFINCH_MULTIPLAYER_INVITE_CODE_LIMIT + 1u];
    char peer_name[TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT + 1u];
    char detail[96];
} TilefinchMultiplayerEvent;

typedef struct {
    uint64_t (*open)(void *context, Budget *budget,
                     const TilefinchMultiplayerRequest *request);
    bool (*send)(void *context, uint64_t session_id,
                 const unsigned char *data, size_t length, bool binary);
    bool (*accept)(void *context, uint64_t session_id,
                   uint32_t peer_identity, bool accepted);
    bool (*add_remote_code)(void *context, uint64_t session_id,
                            const char *invite_code);
    bool (*close)(void *context, uint64_t session_id,
                  uint16_t code, const char *reason);
    bool (*cancel)(void *context, uint64_t session_id);
    bool (*take_event)(void *context, uint64_t session_id,
                       unsigned char *data, size_t capacity,
                       TilefinchMultiplayerEvent *event);
} TilefinchMultiplayerBackend;

/* The frontend installs one process-wide backend before author runtimes can
   request multiplayer. Replacing it while a session is active is refused. */
bool tilefinch_multiplayer_install_backend(
    const TilefinchMultiplayerBackend *backend, void *context);

uint64_t tilefinch_multiplayer_open(
    Budget *budget, const TilefinchMultiplayerRequest *request);
bool tilefinch_multiplayer_send(
    uint64_t session_id, const unsigned char *data, size_t length,
    bool binary);
bool tilefinch_multiplayer_accept(
    uint64_t session_id, uint32_t peer_identity, bool accepted);
bool tilefinch_multiplayer_add_remote_code(
    uint64_t session_id, const char *invite_code);
bool tilefinch_multiplayer_close(
    uint64_t session_id, uint16_t code, const char *reason);
bool tilefinch_multiplayer_cancel(uint64_t session_id);
bool tilefinch_multiplayer_take_event(
    uint64_t session_id, unsigned char *data, size_t capacity,
    TilefinchMultiplayerEvent *event);

/* Invite codes deliberately contain only a routable endpoint and typo
   detection. Peer approval and the post-connect cookie bind the live session
   against unrelated off-path packets; they are not peer authentication. The
   short form omits the preferred port; the fallback includes it. */
bool tilefinch_multiplayer_invite_encode(
    uint32_t ipv4_host_order, uint16_t port, char *output, size_t output_size);
bool tilefinch_multiplayer_invite_decode(
    const char *input, uint32_t *ipv4_host_order, uint16_t *port);

#endif
