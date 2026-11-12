/*
 * WebSocket lane of the PSP transport worker (see background_transport.inc
 * for the threading contract). The includer provides `fetch_background`
 * (its `multi`, `budget`, `initialized` and `websockets` members),
 * background_slot_policy.h, fetch_background_now_us,
 * fetch_background_wake_worker, fetch_configure_tls,
 * curl_easy_attach_shared_state, fetch_configure_private_network and
 * FETCH_BACKGROUND_USER_AGENT. tests/test_websocket_worker_loopback.c
 * includes this file against the pinned libcurl and a loopback server.
 */

static bool fetch_websocket_local_close_code_valid(uint16_t code)
{
    return code == 1000u || (code >= 3000u && code <= 4999u);
}

static uint64_t fetch_background_websocket_make_id(
    unsigned slot, uint32_t generation)
{
    return ((uint64_t) generation << 8u) | (uint64_t) (slot + 1u);
}

static bool fetch_background_websocket_id_slot(
    uint64_t id, unsigned *slot_out, uint32_t *generation_out)
{
    unsigned encoded = (unsigned) (id & UINT64_C(0xff));
    uint64_t generation = id >> 8u;
    if (encoded == 0 || encoded > FETCH_WEBSOCKET_LIMIT
        || generation == 0 || generation > UINT32_MAX) return false;
    if (slot_out != NULL) *slot_out = encoded - 1u;
    if (generation_out != NULL) *generation_out = (uint32_t) generation;
    return true;
}

static FetchBackgroundWebSocket *fetch_background_websocket_for_id(
    uint64_t id)
{
    unsigned index = 0;
    uint32_t generation = 0;
    if (!fetch_background_websocket_id_slot(id, &index, &generation))
        return NULL;
    FetchBackgroundWebSocket *socket = &fetch_background.websockets[index];
    return atomic_load_explicit(
               &socket->generation, memory_order_acquire) == generation
        ? socket : NULL;
}

static void fetch_background_websocket_release_curl(
    FetchBackgroundWebSocket *socket, bool remove_from_multi)
{
    if (socket == NULL) return;
    if (socket->easy != NULL) {
        if (remove_from_multi && socket->in_multi) {
            (void) curl_multi_remove_handle(
                fetch_background.multi, socket->easy);
            socket->in_multi = false;
        }
        curl_easy_cleanup(socket->easy);
        socket->easy = NULL;
    }
    curl_slist_free_all(socket->headers);
    socket->headers = NULL;
}

static void fetch_background_websocket_publish_close_bytes(
    FetchBackgroundWebSocket *socket, uint16_t code,
    const unsigned char *reason, size_t reason_length, bool was_clean)
{
    if (socket == NULL || reason_length > FETCH_WEBSOCKET_CLOSE_REASON_LIMIT
        || (reason_length != 0 && reason == NULL)) return;
    fetch_background_websocket_release_curl(socket, true);
    socket->receive_length = 0;
    socket->close_payload = (FetchWebSocketCloseAccumulator) {0};
    socket->send_length = 0;
    socket->send_offset = 0;
    atomic_store_explicit(
        &socket->tx_state, FETCH_BACKGROUND_WEBSOCKET_TX_EMPTY,
        memory_order_release);
    socket->drain_pending_bytes = 0;
    atomic_store_explicit(
        &socket->drain_state, FETCH_BACKGROUND_WEBSOCKET_DRAIN_EMPTY,
        memory_order_release);
    bool discard = atomic_load_explicit(
        &socket->discard_when_closed, memory_order_acquire) != 0;
    int event_state = atomic_load_explicit(
        &socket->event_state, memory_order_acquire);
    if (!discard && event_state == FETCH_BACKGROUND_WEBSOCKET_EVENT_EMPTY) {
        memset(&socket->event, 0, sizeof(socket->event));
        socket->event.kind = FETCH_WEBSOCKET_EVENT_CLOSE;
        socket->event.close_code = code;
        socket->event.was_clean = was_clean;
        socket->event.reason_length = reason_length;
        if (reason_length != 0)
            memcpy(socket->event.reason, reason, reason_length);
        socket->event.reason[reason_length] = '\0';
        atomic_store_explicit(
            &socket->event_state, FETCH_BACKGROUND_WEBSOCKET_EVENT_READY,
            memory_order_release);
    } else if (!discard) {
        socket->pending_close_code = code;
        socket->pending_close_clean = was_clean;
        socket->pending_close_reason_length = reason_length;
        if (reason_length != 0)
            memcpy(socket->pending_close_reason, reason, reason_length);
        socket->pending_close_reason[reason_length] = '\0';
        atomic_store_explicit(
            &socket->close_pending, 1, memory_order_release);
    } else {
        atomic_store_explicit(
            &socket->close_pending, 0, memory_order_release);
        atomic_store_explicit(
            &socket->event_state, FETCH_BACKGROUND_WEBSOCKET_EVENT_EMPTY,
            memory_order_release);
    }
    atomic_store_explicit(
        &socket->state, FETCH_BACKGROUND_WEBSOCKET_CLOSED,
        memory_order_release);
}

static void fetch_background_websocket_publish_close(
    FetchBackgroundWebSocket *socket, uint16_t code,
    const char *reason, bool was_clean)
{
    const char *safe_reason = reason == NULL ? "" : reason;
    size_t length = strlen(safe_reason);
    if (length > FETCH_WEBSOCKET_CLOSE_REASON_LIMIT)
        length = FETCH_WEBSOCKET_CLOSE_REASON_LIMIT;
    fetch_background_websocket_publish_close_bytes(
        socket, code, (const unsigned char *) safe_reason, length, was_clean);
}

static size_t fetch_background_websocket_receive_header(
    char *data, size_t size, size_t count, void *opaque)
{
    FetchBackgroundWebSocket *socket = opaque;
    static const char name[] = "Sec-WebSocket-Protocol:";
    if (socket == NULL || (size != 0 && count > SIZE_MAX / size)) return 0;
    size_t length = size * count;
    socket->last_progress_us = fetch_background_now_us();
    if (length > sizeof(name) - 1u
        && strncasecmp(data, name, sizeof(name) - 1u) == 0) {
        const char *value = data + sizeof(name) - 1u;
        const char *end = data + length;
        while (value < end && (*value == ' ' || *value == '\t')) value++;
        while (end > value
               && (end[-1] == '\r' || end[-1] == '\n'
                   || end[-1] == ' ' || end[-1] == '\t')) end--;
        size_t value_length = (size_t) (end - value);
        if (value_length == 0
            || value_length >= sizeof(socket->selected_protocol)) return 0;
        memcpy(socket->selected_protocol, value, value_length);
        socket->selected_protocol[value_length] = '\0';
    }
    return length;
}

static bool fetch_background_websocket_append_header(
    FetchBackgroundWebSocket *socket, const char *name, const char *value)
{
    if (socket == NULL || name == NULL || value == NULL) return false;
    char line[FETCH_WEBSOCKET_PROTOCOLS_LIMIT + 32u];
    int written = snprintf(line, sizeof(line), "%s: %s", name, value);
    if (written <= 0 || (size_t) written >= sizeof(line)) return false;
    struct curl_slist *next = curl_slist_append(socket->headers, line);
    if (next == NULL) return false;
    socket->headers = next;
    return true;
}

static bool fetch_background_websocket_protocol_selected(
    const FetchBackgroundWebSocket *socket)
{
    if (socket == NULL) return false;
    if (socket->selected_protocol[0] == '\0') return true;
    const char *at = socket->protocols;
    size_t selected_length = strlen(socket->selected_protocol);
    while (at != NULL && *at != '\0') {
        while (*at == ' ' || *at == '\t' || *at == ',') at++;
        const char *end = strchr(at, ',');
        if (end == NULL) end = at + strlen(at);
        const char *trimmed = end;
        while (trimmed > at
               && (trimmed[-1] == ' ' || trimmed[-1] == '\t')) trimmed--;
        if ((size_t) (trimmed - at) == selected_length
            && memcmp(at, socket->selected_protocol,
                      selected_length) == 0) return true;
        at = *end == '\0' ? NULL : end + 1;
    }
    return false;
}

static bool fetch_background_websocket_configure(
    FetchBackgroundWebSocket *socket)
{
    if (socket == NULL || socket->url[0] == '\0') return false;
    socket->easy = curl_easy_init();
    bool headers_ready = socket->easy != NULL
        && fetch_background_websocket_append_header(
               socket, "Origin", socket->origin)
        && (socket->cookie[0] == '\0'
            || fetch_background_websocket_append_header(
                   socket, "Cookie", socket->cookie))
        && (socket->protocols[0] == '\0'
            || fetch_background_websocket_append_header(
                   socket, "Sec-WebSocket-Protocol", socket->protocols));
    if (!headers_ready
        || !curl_easy_attach_shared_state(socket->easy)
        || !fetch_configure_tls(socket->easy)
        || !fetch_configure_private_network(
               socket->easy, &socket->private_network)
        || curl_easy_setopt(socket->easy, CURLOPT_URL, socket->url)
               != CURLE_OK
        || curl_easy_setopt(socket->easy, CURLOPT_FOLLOWLOCATION, 0L)
               != CURLE_OK
        || curl_easy_setopt(socket->easy, CURLOPT_PROTOCOLS_STR, "ws,wss")
               != CURLE_OK
        || curl_easy_setopt(socket->easy, CURLOPT_REDIR_PROTOCOLS_STR, "ws,wss")
               != CURLE_OK
        || curl_easy_setopt(socket->easy, CURLOPT_CONNECT_ONLY, 2L)
               != CURLE_OK
        || curl_easy_setopt(socket->easy, CURLOPT_HTTPHEADER, socket->headers)
               != CURLE_OK
        || curl_easy_setopt(socket->easy, CURLOPT_USERAGENT,
               socket->user_agent[0] == '\0'
                   ? FETCH_BACKGROUND_USER_AGENT : socket->user_agent)
               != CURLE_OK
        || curl_easy_setopt(socket->easy, CURLOPT_TIMEOUT_MS, 0L) != CURLE_OK
        || curl_easy_setopt(socket->easy, CURLOPT_CONNECTTIMEOUT_MS,
               15000L) != CURLE_OK
        || curl_easy_setopt(socket->easy, CURLOPT_NOSIGNAL, 1L) != CURLE_OK
        || curl_easy_setopt(socket->easy, CURLOPT_ERRORBUFFER,
               socket->curl_error) != CURLE_OK
        || curl_easy_setopt(socket->easy, CURLOPT_HEADERFUNCTION,
               fetch_background_websocket_receive_header) != CURLE_OK
        || curl_easy_setopt(socket->easy, CURLOPT_HEADERDATA, socket)
               != CURLE_OK
        || curl_easy_setopt(socket->easy, CURLOPT_PRIVATE, socket) != CURLE_OK) {
        fetch_background_websocket_release_curl(socket, false);
        return false;
    }
    if (curl_multi_add_handle(fetch_background.multi, socket->easy)
            != CURLM_OK) {
        fetch_background_websocket_release_curl(socket, false);
        return false;
    }
    socket->in_multi = true;
    return true;
}

static void fetch_background_websocket_start_queued(void)
{
    for (unsigned i = 0; i < FETCH_WEBSOCKET_LIMIT; i++) {
        FetchBackgroundWebSocket *socket = &fetch_background.websockets[i];
        if (atomic_load_explicit(&socket->state, memory_order_acquire)
                != FETCH_BACKGROUND_WEBSOCKET_QUEUED) continue;
        if (atomic_load_explicit(
                &socket->cancel_requested, memory_order_acquire)) {
            fetch_background_websocket_publish_close(
                socket, 1006u, "WebSocket cancelled", false);
            continue;
        }
        if (!fetch_background_websocket_configure(socket)) {
            fetch_background_websocket_publish_close(
                socket, 1006u, "WebSocket handshake setup failed", false);
            continue;
        }
        atomic_store_explicit(
            &socket->state, FETCH_BACKGROUND_WEBSOCKET_HANDSHAKING,
            memory_order_release);
    }
}

static bool fetch_background_websocket_finish_handshake(
    CURL *easy, CURLcode code)
{
    FetchBackgroundWebSocket *socket = NULL;
    for (unsigned i = 0; i < FETCH_WEBSOCKET_LIMIT; i++) {
        if (fetch_background.websockets[i].easy == easy) {
            socket = &fetch_background.websockets[i];
            break;
        }
    }
    if (socket == NULL
        || atomic_load_explicit(&socket->state, memory_order_acquire)
               != FETCH_BACKGROUND_WEBSOCKET_HANDSHAKING) return false;
    /* The handle stays in the multi until the socket closes: with
       CURLOPT_CONNECT_ONLY, curl_multi_remove_handle closes the connection
       at once (curl 8.22.0 Curl_multi_remove_handle) and every later
       curl_ws_recv/curl_ws_send fails with "connection not found". A
       finished connect-only handle is idle in curl_multi_perform. */
    if (code != CURLE_OK) {
        const char *reason = socket->curl_error[0] != '\0'
            ? socket->curl_error : curl_easy_strerror(code);
        fetch_background_websocket_publish_close(
            socket, 1006u, reason, false);
        return true;
    }
    if (!fetch_background_websocket_protocol_selected(socket)) {
        fetch_background_websocket_publish_close(
            socket, 1006u, "WebSocket protocol negotiation failed", false);
        return true;
    }
    memset(&socket->event, 0, sizeof(socket->event));
    socket->event.kind = FETCH_WEBSOCKET_EVENT_OPEN;
    snprintf(socket->event.protocol, sizeof(socket->event.protocol), "%s",
             socket->selected_protocol);
    atomic_store_explicit(
        &socket->event_state, FETCH_BACKGROUND_WEBSOCKET_EVENT_READY,
        memory_order_release);
    atomic_store_explicit(
        &socket->state, FETCH_BACKGROUND_WEBSOCKET_OPEN,
        memory_order_release);
    socket->last_progress_us = fetch_background_now_us();
    return true;
}

static void fetch_background_websocket_service_send(
    FetchBackgroundWebSocket *socket)
{
    if (socket == NULL || socket->easy == NULL
        || atomic_load_explicit(&socket->tx_state, memory_order_acquire)
            != FETCH_BACKGROUND_WEBSOCKET_TX_READY) return;
    size_t sent = 0;
    CURLcode code = curl_ws_send(
        socket->easy, socket->send_buffer + socket->send_offset,
        socket->send_length - socket->send_offset, &sent,
        0, (unsigned int) socket->send_flags);
    if (sent != 0) {
        socket->send_offset += sent;
        socket->last_progress_us = fetch_background_now_us();
    }
    if (code == CURLE_AGAIN) return;
    if (code != CURLE_OK || socket->send_offset != socket->send_length) {
        fetch_background_websocket_publish_close(
            socket, 1006u, "WebSocket send failed", false);
        return;
    }
    size_t completed = socket->send_length;
    bool sent_close = (socket->send_flags & CURLWS_CLOSE) != 0;
    socket->send_length = 0;
    socket->send_offset = 0;
    if (sent_close) {
        socket->close_frame_sent = true;
        atomic_store_explicit(
            &socket->tx_state, FETCH_BACKGROUND_WEBSOCKET_TX_EMPTY,
            memory_order_release);
        if (socket->peer_close_received) {
            fetch_background_websocket_publish_close_bytes(
                socket, socket->peer_close_code,
                (const unsigned char *) socket->peer_close_reason,
                socket->peer_close_reason_length, true);
            return;
        }
        socket->deadline_us = fetch_background_now_us() + UINT64_C(2000000);
        atomic_store_explicit(
            &socket->state, FETCH_BACKGROUND_WEBSOCKET_CLOSING,
            memory_order_release);
        return;
    }
    if (socket->peer_close_received) {
        size_t reason_length = socket->peer_close_reason_length;
        if (socket->peer_close_had_code) {
            socket->send_buffer[0] =
                (unsigned char) (socket->peer_close_code >> 8u);
            socket->send_buffer[1] = (unsigned char) socket->peer_close_code;
            if (reason_length != 0)
                memcpy(socket->send_buffer + 2u,
                       socket->peer_close_reason, reason_length);
            socket->send_length = reason_length + 2u;
        } else {
            socket->send_length = 0;
        }
        socket->send_flags = CURLWS_CLOSE;
        atomic_store_explicit(
            &socket->tx_state, FETCH_BACKGROUND_WEBSOCKET_TX_READY,
            memory_order_release);
        return;
    }
    unsigned add = completed > UINT_MAX ? UINT_MAX : (unsigned) completed;
    socket->drain_pending_bytes = add;
    atomic_store_explicit(
        &socket->drain_state, FETCH_BACKGROUND_WEBSOCKET_DRAIN_PENDING,
        memory_order_release);
    atomic_store_explicit(
        &socket->tx_state, FETCH_BACKGROUND_WEBSOCKET_TX_EMPTY,
        memory_order_release);
}

static bool fetch_background_websocket_publish_pending_drain(
    FetchBackgroundWebSocket *socket)
{
    if (socket == NULL
        || atomic_load_explicit(
               &socket->event_state, memory_order_acquire)
               != FETCH_BACKGROUND_WEBSOCKET_EVENT_EMPTY) return false;
    int expected = FETCH_BACKGROUND_WEBSOCKET_DRAIN_PENDING;
    if (!atomic_compare_exchange_strong_explicit(
            &socket->drain_state, &expected,
            FETCH_BACKGROUND_WEBSOCKET_DRAIN_PUBLISHED,
            memory_order_acq_rel, memory_order_acquire)) return false;
    memset(&socket->event, 0, sizeof(socket->event));
    socket->event.kind = FETCH_WEBSOCKET_EVENT_DRAIN;
    socket->event.sent_bytes = socket->drain_pending_bytes;
    atomic_store_explicit(
        &socket->event_state, FETCH_BACKGROUND_WEBSOCKET_EVENT_READY,
        memory_order_release);
    return true;
}

static void fetch_background_websocket_service_receive(
    FetchBackgroundWebSocket *socket)
{
    if (socket == NULL || socket->easy == NULL
        || atomic_load_explicit(
               &socket->event_state, memory_order_acquire)
               != FETCH_BACKGROUND_WEBSOCKET_EVENT_EMPTY) return;
    size_t received = 0;
    const struct curl_ws_frame *frame = NULL;
    unsigned char scratch[4096];
    CURLcode code = curl_ws_recv(
        socket->easy, scratch, sizeof(scratch), &received, &frame);
    if (code == CURLE_AGAIN) return;
    if (code != CURLE_OK || frame == NULL) {
        fetch_background_websocket_publish_close(
            socket, 1006u, "WebSocket receive failed", false);
        return;
    }
    socket->last_progress_us = fetch_background_now_us();
    if ((frame->flags & CURLWS_CLOSE) != 0) {
        FetchWebSocketClosePayload parsed;
        FetchWebSocketClosePayloadStatus status =
            fetch_websocket_close_payload_accumulate(
                &socket->close_payload, scratch, received,
                frame->bytesleft, &parsed);
        /* libcurl may expose a complete control frame over several recv
           calls. Never interpret, echo, or report a partial close payload. */
        if (status == FETCH_WEBSOCKET_CLOSE_PAYLOAD_INCOMPLETE) return;
        if (status != FETCH_WEBSOCKET_CLOSE_PAYLOAD_VALID) {
            fetch_background_websocket_publish_close(
                socket,
                status == FETCH_WEBSOCKET_CLOSE_PAYLOAD_INVALID_UTF8
                    ? 1007u : 1002u,
                status == FETCH_WEBSOCKET_CLOSE_PAYLOAD_INVALID_UTF8
                    ? "Invalid WebSocket close reason"
                    : "Invalid WebSocket close frame",
                false);
            return;
        }
        if (socket->close_frame_sent) {
            fetch_background_websocket_publish_close_bytes(
                socket, parsed.code,
                (const unsigned char *) parsed.reason,
                parsed.reason_length, true);
            return;
        }
        socket->peer_close_code = parsed.code;
        socket->peer_close_had_code = parsed.has_code;
        socket->peer_close_reason_length = parsed.reason_length;
        if (parsed.reason_length != 0) {
            memcpy(socket->peer_close_reason, parsed.reason,
                   parsed.reason_length);
        }
        socket->peer_close_reason[parsed.reason_length] = '\0';
        socket->peer_close_received = true;
        socket->deadline_us =
            fetch_background_now_us() + UINT64_C(2000000);
        atomic_store_explicit(
            &socket->state, FETCH_BACKGROUND_WEBSOCKET_CLOSING,
            memory_order_release);
        int expected = FETCH_BACKGROUND_WEBSOCKET_TX_EMPTY;
        if (atomic_compare_exchange_strong_explicit(
                &socket->tx_state, &expected,
                FETCH_BACKGROUND_WEBSOCKET_TX_PREPARING,
                memory_order_acq_rel, memory_order_acquire)) {
            size_t reason_length = socket->peer_close_reason_length;
            if (socket->peer_close_had_code) {
                socket->send_buffer[0] =
                    (unsigned char) (parsed.code >> 8u);
                socket->send_buffer[1] = (unsigned char) parsed.code;
                if (reason_length != 0)
                    memcpy(socket->send_buffer + 2u,
                           socket->peer_close_reason, reason_length);
                socket->send_length = reason_length + 2u;
            } else {
                socket->send_length = 0;
            }
            socket->send_offset = 0;
            socket->send_flags = CURLWS_CLOSE;
            atomic_store_explicit(
                &socket->tx_state, FETCH_BACKGROUND_WEBSOCKET_TX_READY,
                memory_order_release);
        }
        return;
    }
    if (socket->close_payload.active) {
        fetch_background_websocket_publish_close(
            socket, 1002u, "Interrupted WebSocket close frame", false);
        return;
    }
    /* Hash-pinned curl 8.22.0 auto-enqueues a matching PONG unless the
       unset CURLWS_NOAUTOPONG option disables it. Control frames therefore
       need no page-visible event or second Tilefinch transmit slot. */
    if ((frame->flags & (CURLWS_PING | CURLWS_PONG)) != 0) return;
    if (!socket->receive_started) {
        socket->receive_binary = (frame->flags & CURLWS_BINARY) != 0;
        socket->receive_started = true;
    }
    if (received > FETCH_WEBSOCKET_MESSAGE_LIMIT - socket->receive_length) {
        fetch_background_websocket_publish_close(
            socket, 1009u, "WebSocket message exceeded 64 KiB", false);
        return;
    }
    memcpy(socket->receive_buffer + socket->receive_length,
           scratch, received);
    socket->receive_length += received;
    if (frame->bytesleft != 0 || (frame->flags & CURLWS_CONT) != 0) return;
    if (!socket->receive_binary
        && !fetch_websocket_text_payload_valid(
               socket->receive_buffer, socket->receive_length)) {
        fetch_background_websocket_publish_close(
            socket, 1007u, "Invalid WebSocket text message", false);
        return;
    }
    memset(&socket->event, 0, sizeof(socket->event));
    socket->event.kind = socket->receive_binary
        ? FETCH_WEBSOCKET_EVENT_MESSAGE_BINARY
        : FETCH_WEBSOCKET_EVENT_MESSAGE_TEXT;
    socket->event.length = socket->receive_length;
    socket->receive_started = false;
    atomic_store_explicit(
        &socket->event_state, FETCH_BACKGROUND_WEBSOCKET_EVENT_READY,
        memory_order_release);
}

/* Open sockets are idle connect-only handles in the multi, so curl does not
   poll them itself; the worker adds their sockets to curl_multi_poll. */
static unsigned fetch_background_websocket_poll_fds(
    struct curl_waitfd fds[FETCH_WEBSOCKET_LIMIT])
{
    unsigned count = 0;
    for (unsigned i = 0; i < FETCH_WEBSOCKET_LIMIT; i++) {
        FetchBackgroundWebSocket *socket = &fetch_background.websockets[i];
        int state = atomic_load_explicit(&socket->state, memory_order_acquire);
        if ((state != FETCH_BACKGROUND_WEBSOCKET_OPEN
             && state != FETCH_BACKGROUND_WEBSOCKET_CLOSING)
            || socket->easy == NULL) continue;
        curl_socket_t fd = CURL_SOCKET_BAD;
        if (curl_easy_getinfo(socket->easy, CURLINFO_ACTIVESOCKET, &fd)
                != CURLE_OK
            || fd == CURL_SOCKET_BAD) continue;
        fds[count++] = (struct curl_waitfd) {
            .fd = fd,
            .events = (short) (CURL_WAIT_POLLIN
                | (atomic_load_explicit(
                       &socket->tx_state, memory_order_acquire)
                       == FETCH_BACKGROUND_WEBSOCKET_TX_READY
                    ? CURL_WAIT_POLLOUT : 0)),
            .revents = 0
        };
    }
    return count;
}

/* Queue a page close() that arrived while a send was in flight once the
   transmit slot is free. A peer close that arrived meanwhile wins: its
   echo is the close frame. */
static void fetch_background_websocket_queue_requested_close(
    FetchBackgroundWebSocket *socket)
{
    if (atomic_load_explicit(&socket->close_requested, memory_order_acquire)
            != 1
        || atomic_load_explicit(&socket->state, memory_order_acquire)
               != FETCH_BACKGROUND_WEBSOCKET_OPEN) return;
    int expected = FETCH_BACKGROUND_WEBSOCKET_TX_EMPTY;
    if (!atomic_compare_exchange_strong_explicit(
            &socket->tx_state, &expected,
            FETCH_BACKGROUND_WEBSOCKET_TX_PREPARING,
            memory_order_acq_rel, memory_order_acquire)) return;
    size_t reason_length = socket->requested_close_reason_length;
    socket->send_buffer[0] =
        (unsigned char) (socket->requested_close_code >> 8u);
    socket->send_buffer[1] = (unsigned char) socket->requested_close_code;
    if (reason_length != 0)
        memcpy(socket->send_buffer + 2u, socket->requested_close_reason,
               reason_length);
    socket->send_length = reason_length + 2u;
    socket->send_offset = 0;
    socket->send_flags = CURLWS_CLOSE;
    atomic_store_explicit(&socket->close_requested, 0, memory_order_release);
    atomic_store_explicit(
        &socket->tx_state, FETCH_BACKGROUND_WEBSOCKET_TX_READY,
        memory_order_release);
}

static void fetch_background_websocket_service(void)
{
    uint64_t now_us = fetch_background_now_us();
    for (unsigned i = 0; i < FETCH_WEBSOCKET_LIMIT; i++) {
        FetchBackgroundWebSocket *socket = &fetch_background.websockets[i];
        int state = atomic_load_explicit(&socket->state, memory_order_acquire);
        if (state == FETCH_BACKGROUND_WEBSOCKET_CLOSED
            && atomic_load_explicit(
                   &socket->close_pending, memory_order_acquire)
            && atomic_load_explicit(
                   &socket->event_state, memory_order_acquire)
                   == FETCH_BACKGROUND_WEBSOCKET_EVENT_EMPTY) {
            if (atomic_exchange_explicit(
                    &socket->close_pending, 0, memory_order_acq_rel)) {
                memset(&socket->event, 0, sizeof(socket->event));
                socket->event.kind = FETCH_WEBSOCKET_EVENT_CLOSE;
                socket->event.close_code = socket->pending_close_code;
                socket->event.was_clean = socket->pending_close_clean;
                socket->event.reason_length =
                    socket->pending_close_reason_length;
                if (socket->event.reason_length != 0) {
                    memcpy(socket->event.reason,
                           socket->pending_close_reason,
                           socket->event.reason_length);
                }
                socket->event.reason[socket->event.reason_length] = '\0';
                atomic_store_explicit(
                    &socket->event_state,
                    FETCH_BACKGROUND_WEBSOCKET_EVENT_READY,
                    memory_order_release);
            }
            continue;
        }
        if (state == FETCH_BACKGROUND_WEBSOCKET_HANDSHAKING
            && atomic_load_explicit(
                   &socket->cancel_requested, memory_order_acquire)) {
            fetch_background_websocket_publish_close(
                socket, 1006u, "WebSocket cancelled", false);
            continue;
        }
        if (state == FETCH_BACKGROUND_WEBSOCKET_HANDSHAKING
            && now_us >= socket->deadline_us) {
            fetch_background_websocket_release_curl(socket, true);
            fetch_background_websocket_publish_close(
                socket, 1006u, "WebSocket handshake timed out", false);
            continue;
        }
        if (state != FETCH_BACKGROUND_WEBSOCKET_OPEN
            && state != FETCH_BACKGROUND_WEBSOCKET_CLOSING) continue;
        if (atomic_load_explicit(
                &socket->cancel_requested, memory_order_acquire)) {
            fetch_background_websocket_publish_close(
                socket, 1006u, "WebSocket cancelled", false);
            continue;
        }
        /* A receive/open event can occupy the sole browser handoff while a
           send completes. Publish the accumulated acknowledgement before
           taking more input so bufferedAmount always makes progress. */
        fetch_background_websocket_queue_requested_close(socket);
        if (fetch_background_websocket_publish_pending_drain(socket))
            continue;
        fetch_background_websocket_service_send(socket);
        if (fetch_background_websocket_publish_pending_drain(socket))
            continue;
        state = atomic_load_explicit(&socket->state, memory_order_acquire);
        if ((state == FETCH_BACKGROUND_WEBSOCKET_OPEN
             || state == FETCH_BACKGROUND_WEBSOCKET_CLOSING)
            && socket->easy != NULL
            && atomic_load_explicit(
                   &socket->tx_state, memory_order_acquire)
                   == FETCH_BACKGROUND_WEBSOCKET_TX_EMPTY)
            fetch_background_websocket_service_receive(socket);
        if (atomic_load_explicit(&socket->state, memory_order_acquire)
                == FETCH_BACKGROUND_WEBSOCKET_CLOSING
            && now_us >= socket->deadline_us) {
            fetch_background_websocket_publish_close(
                socket, 1006u, "WebSocket close timed out", false);
        }
    }
}

static void fetch_background_websocket_reap_closed(void)
{
    if (!fetch_background.initialized) return;
    for (unsigned i = 0; i < FETCH_WEBSOCKET_LIMIT; i++) {
        FetchBackgroundWebSocket *socket = &fetch_background.websockets[i];
        if (atomic_load_explicit(&socket->state, memory_order_acquire)
                != FETCH_BACKGROUND_WEBSOCKET_CLOSED
            || atomic_load_explicit(
                   &socket->event_state, memory_order_acquire)
                   != FETCH_BACKGROUND_WEBSOCKET_EVENT_EMPTY
            || atomic_load_explicit(
                   &socket->close_pending, memory_order_acquire)) continue;
        budget_free(fetch_background.budget, socket->receive_buffer);
        budget_free(fetch_background.budget, socket->send_buffer);
        socket->receive_buffer = NULL;
        socket->send_buffer = NULL;
        atomic_store_explicit(
            &socket->state, FETCH_BACKGROUND_WEBSOCKET_FREE,
            memory_order_release);
    }
}

/* Claim a free socket for an already admitted target (see
   fetch_background_websocket_open for the authority checks) and queue its
   handshake on the worker. Field lengths were checked by the caller. */
static uint64_t fetch_background_websocket_claim(
    Budget *budget, const char *admitted_url, bool protect_private_network,
    const char *origin, const char *cookie, const char *protocols,
    const char *user_agent, long timeout_ms)
{
    fetch_background_websocket_reap_closed();
    for (unsigned i = 0; i < FETCH_WEBSOCKET_LIMIT; i++) {
        FetchBackgroundWebSocket *socket = &fetch_background.websockets[i];
        int expected = FETCH_BACKGROUND_WEBSOCKET_FREE;
        if (!atomic_compare_exchange_strong_explicit(
                &socket->state, &expected,
                FETCH_BACKGROUND_WEBSOCKET_PREPARING,
                memory_order_acq_rel, memory_order_acquire)) continue;
        unsigned char *receive = budget_malloc_category(
            budget, BUDGET_CATEGORY_RESOURCE,
            FETCH_WEBSOCKET_MESSAGE_LIMIT);
        unsigned char *send = receive == NULL ? NULL : budget_malloc_category(
            budget, BUDGET_CATEGORY_RESOURCE,
            FETCH_WEBSOCKET_MESSAGE_LIMIT);
        if (send == NULL) {
            budget_free(budget, receive);
            atomic_store_explicit(
                &socket->state, FETCH_BACKGROUND_WEBSOCKET_FREE,
                memory_order_release);
            return 0;
        }
        uint32_t generation = fetch_background_generation_next(
            atomic_load_explicit(
                &socket->generation, memory_order_relaxed));
        socket->receive_buffer = receive;
        socket->send_buffer = send;
        socket->receive_length = 0;
        socket->send_length = 0;
        socket->send_offset = 0;
        socket->send_flags = 0;
        socket->receive_binary = false;
        socket->receive_started = false;
        socket->peer_close_received = false;
        socket->peer_close_had_code = false;
        socket->close_frame_sent = false;
        socket->peer_close_code = 1005u;
        socket->peer_close_reason_length = 0;
        socket->peer_close_reason[0] = '\0';
        socket->close_payload = (FetchWebSocketCloseAccumulator) {0};
        socket->pending_close_reason_length = 0;
        socket->pending_close_reason[0] = '\0';
        socket->selected_protocol[0] = '\0';
        socket->curl_error[0] = '\0';
        socket->easy = NULL;
        socket->headers = NULL;
        socket->in_multi = false;
        socket->private_network.required = protect_private_network;
        socket->private_network.blocked = false;
        snprintf(socket->url, sizeof(socket->url), "%s", admitted_url);
        snprintf(socket->origin, sizeof(socket->origin), "%s",
                 origin);
        snprintf(socket->cookie, sizeof(socket->cookie), "%s",
                 cookie == NULL ? "" : cookie);
        snprintf(socket->protocols, sizeof(socket->protocols), "%s",
                 protocols == NULL ? "" : protocols);
        snprintf(socket->user_agent, sizeof(socket->user_agent), "%s",
                 user_agent == NULL ? "" : user_agent);
        uint64_t now_us = fetch_background_now_us();
        uint64_t duration = (uint64_t) timeout_ms * UINT64_C(1000);
        socket->deadline_us = duration > UINT64_MAX - now_us
            ? UINT64_MAX : now_us + duration;
        socket->last_progress_us = now_us;
        memset(&socket->event, 0, sizeof(socket->event));
        atomic_store_explicit(
            &socket->generation, generation, memory_order_release);
        atomic_store_explicit(
            &socket->cancel_requested, 0, memory_order_release);
        atomic_store_explicit(
            &socket->close_pending, 0, memory_order_release);
        atomic_store_explicit(
            &socket->close_requested, 0, memory_order_release);
        socket->drain_pending_bytes = 0;
        atomic_store_explicit(
            &socket->drain_state, FETCH_BACKGROUND_WEBSOCKET_DRAIN_EMPTY,
            memory_order_release);
        atomic_store_explicit(
            &socket->discard_when_closed, 0, memory_order_release);
        atomic_store_explicit(
            &socket->tx_state, FETCH_BACKGROUND_WEBSOCKET_TX_EMPTY,
            memory_order_release);
        atomic_store_explicit(
            &socket->event_state, FETCH_BACKGROUND_WEBSOCKET_EVENT_EMPTY,
            memory_order_release);
        atomic_store_explicit(
            &socket->state, FETCH_BACKGROUND_WEBSOCKET_QUEUED,
            memory_order_release);
        fetch_background_wake_worker();
        return fetch_background_websocket_make_id(i, generation);
    }
    return 0;
}

bool fetch_background_websocket_send(
    uint64_t socket_id, const unsigned char *data, size_t length,
    bool binary)
{
    FetchBackgroundWebSocket *socket =
        fetch_background_websocket_for_id(socket_id);
    if (socket == NULL || (length != 0 && data == NULL)
        || length > FETCH_WEBSOCKET_MESSAGE_LIMIT
        || atomic_load_explicit(&socket->state, memory_order_acquire)
               != FETCH_BACKGROUND_WEBSOCKET_OPEN
        || atomic_load_explicit(&socket->drain_state, memory_order_acquire)
               != FETCH_BACKGROUND_WEBSOCKET_DRAIN_EMPTY) return false;
    int expected = FETCH_BACKGROUND_WEBSOCKET_TX_EMPTY;
    if (!atomic_compare_exchange_strong_explicit(
            &socket->tx_state, &expected,
            FETCH_BACKGROUND_WEBSOCKET_TX_PREPARING,
            memory_order_acq_rel, memory_order_acquire)) return false;
    if (atomic_load_explicit(&socket->drain_state, memory_order_acquire)
            != FETCH_BACKGROUND_WEBSOCKET_DRAIN_EMPTY) {
        atomic_store_explicit(
            &socket->tx_state, FETCH_BACKGROUND_WEBSOCKET_TX_EMPTY,
            memory_order_release);
        return false;
    }
    if (length != 0) memcpy(socket->send_buffer, data, length);
    socket->send_length = length;
    socket->send_offset = 0;
    socket->send_flags = binary ? CURLWS_BINARY : CURLWS_TEXT;
    atomic_store_explicit(
        &socket->tx_state, FETCH_BACKGROUND_WEBSOCKET_TX_READY,
        memory_order_release);
    fetch_background_wake_worker();
    return true;
}

bool fetch_background_websocket_close(
    uint64_t socket_id, uint16_t code, const char *reason)
{
    FetchBackgroundWebSocket *socket =
        fetch_background_websocket_for_id(socket_id);
    size_t reason_length = reason == NULL ? 0u : strlen(reason);
    int state = socket == NULL ? FETCH_BACKGROUND_WEBSOCKET_FREE
        : atomic_load_explicit(&socket->state, memory_order_acquire);
    if (socket == NULL || !fetch_websocket_local_close_code_valid(code)
        || reason_length > FETCH_WEBSOCKET_CLOSE_REASON_LIMIT
        || !fetch_websocket_text_payload_valid(
               (const unsigned char *) reason, reason_length)
        || (state != FETCH_BACKGROUND_WEBSOCKET_OPEN
            && state != FETCH_BACKGROUND_WEBSOCKET_QUEUED
            && state != FETCH_BACKGROUND_WEBSOCKET_HANDSHAKING
            && state != FETCH_BACKGROUND_WEBSOCKET_CLOSING
            && state != FETCH_BACKGROUND_WEBSOCKET_CLOSED)) return false;
    /* The closing handshake is already under way (a peer close being
       echoed, or our own close frame sent): the close event follows, as
       close() in the CLOSING state does nothing in the WebSocket API. */
    if (state == FETCH_BACKGROUND_WEBSOCKET_CLOSING
        || state == FETCH_BACKGROUND_WEBSOCKET_CLOSED
        || atomic_load_explicit(
               &socket->close_requested, memory_order_acquire) != 0)
        return true;
    if (state != FETCH_BACKGROUND_WEBSOCKET_OPEN) {
        atomic_store_explicit(
            &socket->cancel_requested, 1, memory_order_release);
    } else {
        int expected = FETCH_BACKGROUND_WEBSOCKET_TX_EMPTY;
        if (!atomic_compare_exchange_strong_explicit(
                &socket->tx_state, &expected,
                FETCH_BACKGROUND_WEBSOCKET_TX_PREPARING,
                memory_order_acq_rel, memory_order_acquire)) {
            /* A send is in flight. Leave the close frame for the worker to
               queue behind it rather than failing the close (which left
               the socket open when a Worker was torn down). */
            int idle = 0;
            if (!atomic_compare_exchange_strong_explicit(
                    &socket->close_requested, &idle, 2,
                    memory_order_acq_rel, memory_order_acquire))
                return true;
            socket->requested_close_code = code;
            socket->requested_close_reason_length = reason_length;
            if (reason_length != 0)
                memcpy(socket->requested_close_reason, reason, reason_length);
            socket->requested_close_reason[reason_length] = '\0';
            atomic_store_explicit(
                &socket->close_requested, 1, memory_order_release);
            fetch_background_wake_worker();
            return true;
        }
        socket->send_buffer[0] = (unsigned char) (code >> 8u);
        socket->send_buffer[1] = (unsigned char) code;
        if (reason_length != 0)
            memcpy(socket->send_buffer + 2u, reason, reason_length);
        socket->send_length = reason_length + 2u;
        socket->send_offset = 0;
        socket->send_flags = CURLWS_CLOSE;
        atomic_store_explicit(
            &socket->tx_state, FETCH_BACKGROUND_WEBSOCKET_TX_READY,
            memory_order_release);
    }
    fetch_background_wake_worker();
    return true;
}

bool fetch_background_websocket_cancel(uint64_t socket_id)
{
    FetchBackgroundWebSocket *socket =
        fetch_background_websocket_for_id(socket_id);
    if (socket == NULL) return false;
    atomic_store_explicit(
        &socket->discard_when_closed, 1, memory_order_release);
    atomic_store_explicit(
        &socket->cancel_requested, 1, memory_order_release);
    if (atomic_load_explicit(&socket->state, memory_order_acquire)
            == FETCH_BACKGROUND_WEBSOCKET_CLOSED) {
        atomic_store_explicit(
            &socket->close_pending, 0, memory_order_release);
        atomic_store_explicit(
            &socket->event_state, FETCH_BACKGROUND_WEBSOCKET_EVENT_EMPTY,
            memory_order_release);
        fetch_background_websocket_reap_closed();
    } else {
        fetch_background_wake_worker();
    }
    return true;
}

bool fetch_background_websocket_abort(uint64_t socket_id)
{
    FetchBackgroundWebSocket *socket =
        fetch_background_websocket_for_id(socket_id);
    if (socket == NULL) return false;
    atomic_store_explicit(
        &socket->cancel_requested, 1, memory_order_release);
    fetch_background_wake_worker();
    return true;
}

bool fetch_background_websocket_take_event(
    uint64_t socket_id, unsigned char *data, size_t capacity,
    FetchWebSocketEvent *event)
{
    FetchBackgroundWebSocket *socket =
        fetch_background_websocket_for_id(socket_id);
    if (socket == NULL || event == NULL) return false;
    int expected = FETCH_BACKGROUND_WEBSOCKET_EVENT_READY;
    if (!atomic_compare_exchange_strong_explicit(
            &socket->event_state, &expected,
            FETCH_BACKGROUND_WEBSOCKET_EVENT_TAKING,
            memory_order_acq_rel, memory_order_acquire)) return false;
    *event = socket->event;
    if ((event->kind == FETCH_WEBSOCKET_EVENT_MESSAGE_TEXT
         || event->kind == FETCH_WEBSOCKET_EVENT_MESSAGE_BINARY)
        && (event->length > capacity
            || (event->length != 0 && data == NULL))) {
        atomic_store_explicit(
            &socket->event_state, FETCH_BACKGROUND_WEBSOCKET_EVENT_READY,
            memory_order_release);
        return false;
    }
    if (event->length != 0)
        memcpy(data, socket->receive_buffer, event->length);
    if (event->kind == FETCH_WEBSOCKET_EVENT_MESSAGE_TEXT
        || event->kind == FETCH_WEBSOCKET_EVENT_MESSAGE_BINARY)
        socket->receive_length = 0;
    if (event->kind == FETCH_WEBSOCKET_EVENT_DRAIN) {
        atomic_store_explicit(
            &socket->drain_state, FETCH_BACKGROUND_WEBSOCKET_DRAIN_EMPTY,
            memory_order_release);
    }
    atomic_store_explicit(
        &socket->event_state, FETCH_BACKGROUND_WEBSOCKET_EVENT_EMPTY,
        memory_order_release);
    if (atomic_load_explicit(
            &socket->close_pending, memory_order_acquire)
        || atomic_load_explicit(
            &socket->drain_state, memory_order_acquire)
               == FETCH_BACKGROUND_WEBSOCKET_DRAIN_PENDING) {
        fetch_background_wake_worker();
    }
    if (event->kind == FETCH_WEBSOCKET_EVENT_CLOSE)
        fetch_background_websocket_reap_closed();
    return true;
}
