/*
 * WebSocket socket descriptor shared by the PSP transport worker and the
 * browser thread. Included by background_transport.inc (PSP owned transport)
 * and by tests/test_websocket_worker_loopback.c; the includer defines the
 * FETCH_BACKGROUND_*_CAPACITY limits and FetchPrivateNetworkGuard first.
 */

typedef enum {
    FETCH_BACKGROUND_WEBSOCKET_FREE = 0,
    FETCH_BACKGROUND_WEBSOCKET_PREPARING,
    FETCH_BACKGROUND_WEBSOCKET_QUEUED,
    FETCH_BACKGROUND_WEBSOCKET_HANDSHAKING,
    FETCH_BACKGROUND_WEBSOCKET_OPEN,
    FETCH_BACKGROUND_WEBSOCKET_CLOSING,
    FETCH_BACKGROUND_WEBSOCKET_CLOSED
} FetchBackgroundWebSocketState;

typedef enum {
    FETCH_BACKGROUND_WEBSOCKET_EVENT_EMPTY = 0,
    FETCH_BACKGROUND_WEBSOCKET_EVENT_READY,
    FETCH_BACKGROUND_WEBSOCKET_EVENT_TAKING
} FetchBackgroundWebSocketEventState;

typedef enum {
    FETCH_BACKGROUND_WEBSOCKET_TX_EMPTY = 0,
    FETCH_BACKGROUND_WEBSOCKET_TX_PREPARING,
    FETCH_BACKGROUND_WEBSOCKET_TX_READY
} FetchBackgroundWebSocketTxState;

typedef enum {
    FETCH_BACKGROUND_WEBSOCKET_DRAIN_EMPTY = 0,
    FETCH_BACKGROUND_WEBSOCKET_DRAIN_PENDING,
    FETCH_BACKGROUND_WEBSOCKET_DRAIN_PUBLISHED
} FetchBackgroundWebSocketDrainState;

typedef struct {
    atomic_int state;
    atomic_int event_state;
    atomic_int tx_state;
    atomic_int cancel_requested;
    atomic_int close_pending;
    atomic_int drain_state;
    atomic_uint generation;
    CURL *easy;
    struct curl_slist *headers;
    bool in_multi;
    unsigned char *receive_buffer;
    unsigned char *send_buffer;
    size_t receive_length;
    size_t send_length;
    size_t send_offset;
    int send_flags;
    unsigned drain_pending_bytes;
    bool receive_binary;
    bool receive_started;
    bool peer_close_received;
    bool peer_close_had_code;
    bool close_frame_sent;
    atomic_int discard_when_closed;
    uint16_t peer_close_code;
    size_t peer_close_reason_length;
    char peer_close_reason[FETCH_WEBSOCKET_CLOSE_REASON_LIMIT + 1u];
    FetchWebSocketCloseAccumulator close_payload;
    uint16_t pending_close_code;
    bool pending_close_clean;
    size_t pending_close_reason_length;
    char pending_close_reason[FETCH_WEBSOCKET_CLOSE_REASON_LIMIT + 1u];
    /* A page close() that found a send in flight: the worker sends this
       close frame once the transmit slot is free (1 = ready, 2 = being
       written by the browser thread). */
    atomic_int close_requested;
    uint16_t requested_close_code;
    size_t requested_close_reason_length;
    char requested_close_reason[FETCH_WEBSOCKET_CLOSE_REASON_LIMIT + 1u];
    FetchWebSocketEvent event;
    uint64_t deadline_us;
    uint64_t last_progress_us;
    FetchPrivateNetworkGuard private_network;
    char url[FETCH_BACKGROUND_URL_CAPACITY];
    char origin[FETCH_BACKGROUND_ORIGIN_CAPACITY];
    char cookie[FETCH_BACKGROUND_COOKIE_CAPACITY];
    char protocols[FETCH_WEBSOCKET_PROTOCOLS_LIMIT];
    char selected_protocol[128];
    char user_agent[FETCH_BACKGROUND_USER_AGENT_CAPACITY];
    char curl_error[CURL_ERROR_SIZE];
} FetchBackgroundWebSocket;
