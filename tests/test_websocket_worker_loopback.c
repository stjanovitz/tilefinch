/*
 * Loopback test of the PSP transport worker's WebSocket lane.
 *
 * Compiles src/fetch/background_websocket.inc (the code the PSP worker runs)
 * against host builds of the pinned libcurl 8.22.0 and Mbed TLS 3.6.7 and
 * drives it, in the
 * worker's own order (perform, collect CURLMSG_DONE, start queued, service,
 * poll), against a loopback RFC 6455 server on a second thread:
 *
 *   1. handshake -> OPEN event;
 *   2. send a text message -> DRAIN event, then the echoed MESSAGE event
 *      (NW-01: the handshake handle used to be removed from the multi, which
 *      closes a connect-only connection; the socket then died with 1006);
 *   3. the server starts the closing handshake; a page close() issued while
 *      the worker is CLOSING is accepted (NW-10) and the socket ends with the
 *      server's clean 1000 close; the server sees our close echo;
 *   4. a close() issued while a send is still in flight is accepted (NW-10)
 *      and the socket closes cleanly behind the message;
 *   5. every Budget byte is returned.
 *
 * Plain ws:// only: the pinned curl is built with Mbed TLS because the
 * lane admits "ws,wss", but TLS is not part of what is tested here.
 */
#include <curl/curl.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "tilefinch/fetch.h"
#include "fetch/background_slot_policy.h"

static int failures;
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "websocket-loopback failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #value); failures++; \
} } while (0)

/* ---- What background_transport.inc provides to the WebSocket lane. ---- */

#define FETCH_BACKGROUND_URL_CAPACITY 4160u
#define FETCH_BACKGROUND_COOKIE_CAPACITY 1024u
#define FETCH_BACKGROUND_ORIGIN_CAPACITY 2048u
#define FETCH_BACKGROUND_USER_AGENT_CAPACITY 512u
#define FETCH_BACKGROUND_USER_AGENT "TilefinchWebSocketLoopbackTest"

typedef struct {
    bool required;
    bool blocked;
} FetchPrivateNetworkGuard;

#include "fetch/background_websocket_types.inc"

static struct {
    Budget *budget;
    CURLM *multi;
    bool initialized;
    FetchBackgroundWebSocket websockets[FETCH_WEBSOCKET_LIMIT];
} fetch_background;

static CURLSH *test_share;
static size_t test_budget_outstanding;
static unsigned test_wakes;

void *(budget_malloc_category)(Budget *budget, BudgetCategory category,
                               size_t size)
{
    (void) budget;
    (void) category;
    void *result = malloc(size);
    if (result != NULL) test_budget_outstanding++;
    return result;
}

void budget_free(Budget *budget, void *ptr)
{
    (void) budget;
    if (ptr == NULL) return;
    test_budget_outstanding--;
    free(ptr);
}

static uint64_t fetch_background_now_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * UINT64_C(1000000)
        + (uint64_t) now.tv_nsec / UINT64_C(1000);
}

static void fetch_background_wake_worker(void)
{
    test_wakes++;
    (void) curl_multi_wakeup(fetch_background.multi);
}

static bool fetch_configure_tls(CURL *easy)
{
    (void) easy;
    return true;
}

/* Tilefinch's share holds the DNS cache and the connection pool. */
static bool curl_easy_attach_shared_state(CURL *easy)
{
    return curl_easy_setopt(easy, CURLOPT_SHARE, test_share) == CURLE_OK;
}

static bool fetch_configure_private_network(
    CURL *easy, FetchPrivateNetworkGuard *guard)
{
    (void) easy;
    return guard != NULL;
}

#include "fetch/background_websocket.inc"

/* ---- Loopback RFC 6455 server. ---- */

typedef struct {
    int listener;
    unsigned short port;
    atomic_int saw_close_echo;
    atomic_int close_echo_code;
    atomic_int page_close_code;
    atomic_int failed;
} TestServer;

static uint32_t sha1_rotl(uint32_t value, unsigned bits)
{
    return (value << bits) | (value >> (32u - bits));
}

static void sha1(const unsigned char *data, size_t length,
                 unsigned char out[20])
{
    uint32_t h[5] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu,
                      0x10325476u, 0xc3d2e1f0u };
    unsigned char block[64];
    uint64_t bits = (uint64_t) length * 8u;
    size_t total = ((length + 8u) / 64u + 1u) * 64u;
    for (size_t offset = 0; offset < total; offset += 64u) {
        for (size_t i = 0; i < 64u; i++) {
            size_t at = offset + i;
            block[i] = at < length ? data[at]
                : at == length ? 0x80u
                : at >= total - 8u
                    ? (unsigned char) (bits >> (8u * (total - 1u - at)))
                    : 0u;
        }
        uint32_t w[80];
        for (unsigned i = 0; i < 16u; i++) {
            w[i] = (uint32_t) block[i * 4u] << 24u
                | (uint32_t) block[i * 4u + 1u] << 16u
                | (uint32_t) block[i * 4u + 2u] << 8u
                | (uint32_t) block[i * 4u + 3u];
        }
        for (unsigned i = 16u; i < 80u; i++)
            w[i] = sha1_rotl(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u],
                             1u);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (unsigned i = 0; i < 80u; i++) {
            uint32_t f, k;
            if (i < 20u) { f = (b & c) | (~b & d); k = 0x5a827999u; }
            else if (i < 40u) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
            else if (i < 60u) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
            else { f = b ^ c ^ d; k = 0xca62c1d6u; }
            uint32_t next = sha1_rotl(a, 5u) + f + e + k + w[i];
            e = d; d = c; c = sha1_rotl(b, 30u); b = a; a = next;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (unsigned i = 0; i < 5u; i++) {
        out[i * 4u] = (unsigned char) (h[i] >> 24u);
        out[i * 4u + 1u] = (unsigned char) (h[i] >> 16u);
        out[i * 4u + 2u] = (unsigned char) (h[i] >> 8u);
        out[i * 4u + 3u] = (unsigned char) h[i];
    }
}

static void base64(const unsigned char *data, size_t length, char *out)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < length; i += 3u) {
        uint32_t v = (uint32_t) data[i] << 16u;
        if (i + 1u < length) v |= (uint32_t) data[i + 1u] << 8u;
        if (i + 2u < length) v |= data[i + 2u];
        out[o++] = alphabet[(v >> 18u) & 63u];
        out[o++] = alphabet[(v >> 12u) & 63u];
        out[o++] = i + 1u < length ? alphabet[(v >> 6u) & 63u] : '=';
        out[o++] = i + 2u < length ? alphabet[v & 63u] : '=';
    }
    out[o] = '\0';
}

static bool read_exact(int fd, unsigned char *buffer, size_t length)
{
    size_t at = 0;
    while (at < length) {
        ssize_t got = recv(fd, buffer + at, length - at, 0);
        if (got <= 0) return false;
        at += (size_t) got;
    }
    return true;
}

static bool send_all(int fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    while (length != 0) {
        ssize_t sent = send(fd, bytes, length, 0);
        if (sent <= 0) return false;
        bytes += sent;
        length -= (size_t) sent;
    }
    return true;
}

/* One client frame (masked, payload under 126 bytes). */
static bool read_client_frame(int fd, unsigned *opcode,
                              unsigned char *payload, size_t *length)
{
    unsigned char head[2], mask[4];
    if (!read_exact(fd, head, 2u) || (head[1] & 0x80u) == 0
        || (head[1] & 0x7fu) >= 126u || !read_exact(fd, mask, 4u))
        return false;
    *opcode = head[0] & 0x0fu;
    *length = head[1] & 0x7fu;
    if (!read_exact(fd, payload, *length)) return false;
    for (size_t i = 0; i < *length; i++) payload[i] ^= mask[i & 3u];
    return true;
}

/* Connection 1: echo one message, then close 1000 "bye" and wait for the
   echo. Connection 2: echo one message, then answer the client's close. */
static bool serve_connection(TestServer *server, unsigned connection)
{
    int fd = accept(server->listener, NULL, NULL);
    if (fd < 0) return false;
    /* Never outlive a failed client: every read gives up after 5 s. */
    struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
    (void) setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char request[4096];
    size_t used = 0;
    while (used < sizeof(request) - 1u) {
        ssize_t got = recv(fd, request + used, sizeof(request) - 1u - used, 0);
        if (got <= 0) break;
        used += (size_t) got;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n") != NULL) break;
    }
    request[used] = '\0';
    const char *key = strcasestr(request, "Sec-WebSocket-Key:");
    char key_value[64] = "";
    if (key != NULL) {
        key += strlen("Sec-WebSocket-Key:");
        while (*key == ' ') key++;
        size_t n = strcspn(key, "\r\n");
        if (n < sizeof(key_value)) {
            memcpy(key_value, key, n);
            key_value[n] = '\0';
        }
    }
    char concatenated[128];
    snprintf(concatenated, sizeof(concatenated), "%s%s", key_value,
             "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    unsigned char digest[20];
    sha1((const unsigned char *) concatenated, strlen(concatenated), digest);
    char accept_value[32];
    base64(digest, sizeof(digest), accept_value);
    char response[256];
    int response_length = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
        accept_value);
    unsigned opcode = 0;
    unsigned char payload[128];
    size_t length = 0;
    bool ok = key_value[0] != '\0'
        && send_all(fd, response, (size_t) response_length)
        && read_client_frame(fd, &opcode, payload, &length)
        && opcode == 0x1u;
    if (ok) {
        unsigned char echo[2 + 125];
        echo[0] = 0x81u;
        echo[1] = (unsigned char) length;
        memcpy(echo + 2, payload, length);
        ok = send_all(fd, echo, 2u + length);
    }
    if (connection == 1u) {
        /* Start the closing handshake: 1000 "bye". */
        static const unsigned char close_frame[] = {
            0x88u, 0x05u, 0x03u, 0xe8u, 'b', 'y', 'e' };
        if (ok) ok = send_all(fd, close_frame, sizeof(close_frame));
        if (ok && read_client_frame(fd, &opcode, payload, &length)
            && opcode == 0x8u) {
            atomic_store(&server->saw_close_echo, 1);
            atomic_store(&server->close_echo_code, length >= 2u
                ? (int) ((payload[0] << 8u) | payload[1]) : 1005);
        }
    } else if (ok) {
        ok = read_client_frame(fd, &opcode, payload, &length)
            && opcode == 0x8u && length >= 2u;
        if (ok) {
            atomic_store(&server->page_close_code,
                         (int) ((payload[0] << 8u) | payload[1]));
            unsigned char echo[2 + 125];
            echo[0] = 0x88u;
            echo[1] = (unsigned char) length;
            memcpy(echo + 2, payload, length);
            ok = send_all(fd, echo, 2u + length);
        }
    }
    close(fd);
    return ok;
}

static void *server_main(void *opaque)
{
    TestServer *server = opaque;
    for (unsigned connection = 1u; connection <= 2u; connection++) {
        if (!serve_connection(server, connection)) {
            atomic_store(&server->failed, 1);
            break;
        }
    }
    return NULL;
}

static bool server_start(TestServer *server)
{
    memset(server, 0, sizeof(*server));
    server->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener < 0) return false;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t size = sizeof(address);
    if (bind(server->listener, (struct sockaddr *) &address, sizeof(address))
            != 0
        || listen(server->listener, 1) != 0
        || getsockname(server->listener, (struct sockaddr *) &address, &size)
               != 0) {
        close(server->listener);
        return false;
    }
    server->port = ntohs(address.sin_port);
    return true;
}

/* ---- The worker loop, in fetch_background_worker_main's order. ---- */

static void worker_step(void)
{
    int running = 0;
    (void) curl_multi_perform(fetch_background.multi, &running);
    int remaining = 0;
    CURLMsg *message = NULL;
    while ((message = curl_multi_info_read(
                fetch_background.multi, &remaining)) != NULL) {
        if (message->msg != CURLMSG_DONE) continue;
        CHECK(fetch_background_websocket_finish_handshake(
            message->easy_handle, message->data.result));
    }
    fetch_background_websocket_start_queued();
    fetch_background_websocket_service();
    struct curl_waitfd fds[FETCH_WEBSOCKET_LIMIT];
    unsigned count = fetch_background_websocket_poll_fds(fds);
    (void) curl_multi_poll(fetch_background.multi, fds, count, 10, NULL);
}

/* Run the worker until the socket publishes an event, at most ~5 s. */
static bool take_event(uint64_t id, unsigned char *data, size_t capacity,
                       FetchWebSocketEvent *event)
{
    for (unsigned turn = 0; turn < 500u; turn++) {
        if (fetch_background_websocket_take_event(id, data, capacity, event))
            return true;
        worker_step();
    }
    return false;
}

static const char *event_name(FetchWebSocketEventKind kind)
{
    switch (kind) {
    case FETCH_WEBSOCKET_EVENT_OPEN: return "open";
    case FETCH_WEBSOCKET_EVENT_MESSAGE_TEXT: return "text";
    case FETCH_WEBSOCKET_EVENT_MESSAGE_BINARY: return "binary";
    case FETCH_WEBSOCKET_EVENT_DRAIN: return "drain";
    case FETCH_WEBSOCKET_EVENT_CLOSE: return "close";
    default: return "none";
    }
}

int main(void)
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    test_share = curl_share_init();
    fetch_background.multi = curl_multi_init();
    CHECK(test_share != NULL && fetch_background.multi != NULL);
    (void) curl_share_setopt(test_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    (void) curl_share_setopt(
        test_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    Budget budget;
    memset(&budget, 0, sizeof(budget));
    fetch_background.budget = &budget;
    fetch_background.initialized = true;

    TestServer server;
    if (!server_start(&server)) {
        fprintf(stderr, "websocket-loopback: cannot listen on loopback\n");
        return 1;
    }
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, server_main, &server) == 0);
    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%u/echo",
             (unsigned) server.port);

    uint64_t id = fetch_background_websocket_claim(
        &budget, url, false, "http://127.0.0.1", "", "", "", 5000);
    CHECK(id != 0);
    unsigned char data[FETCH_WEBSOCKET_MESSAGE_LIMIT];
    FetchWebSocketEvent event;

    /* 1. Handshake. */
    bool opened = take_event(id, data, sizeof(data), &event)
        && event.kind == FETCH_WEBSOCKET_EVENT_OPEN;
    CHECK(opened);
    if (!opened) {
        FetchBackgroundWebSocket *failed =
            fetch_background_websocket_for_id(id);
        fprintf(stderr, "websocket-loopback: no open (state %d, event %s "
                "%u \"%s\")\n",
                failed == NULL ? -1 : atomic_load(&failed->state),
                event_name(event.kind), (unsigned) event.close_code,
                event.reason);
    }

    /* 2. One message each way after open. */
    static const char hello[] = "hello after open";
    CHECK(fetch_background_websocket_send(
        id, (const unsigned char *) hello, sizeof(hello) - 1u, false));
    bool drained = false, echoed = false;
    for (unsigned events = 0; events < 4u && !(drained && echoed); events++) {
        if (!take_event(id, data, sizeof(data), &event)) break;
        if (event.kind == FETCH_WEBSOCKET_EVENT_DRAIN) {
            drained = event.sent_bytes == sizeof(hello) - 1u;
        } else if (event.kind == FETCH_WEBSOCKET_EVENT_MESSAGE_TEXT) {
            echoed = event.length == sizeof(hello) - 1u
                && memcmp(data, hello, event.length) == 0;
        } else {
            fprintf(stderr, "websocket-loopback: unexpected %s event "
                    "(code %u, \"%s\")\n", event_name(event.kind),
                    (unsigned) event.close_code, event.reason);
            break;
        }
    }
    CHECK(drained);
    CHECK(echoed);

    /* 3. Peer-initiated close; the page closes while the worker is
       CLOSING. */
    FetchBackgroundWebSocket *socket = fetch_background_websocket_for_id(id);
    CHECK(socket != NULL);
    bool closing = false;
    for (unsigned turn = 0; socket != NULL && turn < 500u && !closing;
         turn++) {
        worker_step();
        closing = atomic_load(&socket->state)
            == FETCH_BACKGROUND_WEBSOCKET_CLOSING;
        if (atomic_load(&socket->state) == FETCH_BACKGROUND_WEBSOCKET_CLOSED)
            break;
    }
    CHECK(closing);
    if (closing)
        CHECK(fetch_background_websocket_close(id, 1000u, "page close"));
    CHECK(take_event(id, data, sizeof(data), &event)
          && event.kind == FETCH_WEBSOCKET_EVENT_CLOSE);
    CHECK(event.close_code == 1000u && event.was_clean
          && strcmp(event.reason, "bye") == 0);

    CHECK(atomic_load(&server.saw_close_echo) == 1);
    CHECK(atomic_load(&server.close_echo_code) == 1000);
    CHECK(test_budget_outstanding == 0);

    /* 4. A close() issued while a send is still in flight (as a Worker's
       teardown does) is accepted and goes out behind the message (NW-10). */
    uint64_t second = fetch_background_websocket_claim(
        &budget, url, false, "http://127.0.0.1", "", "", "", 5000);
    CHECK(second != 0);
    CHECK(take_event(second, data, sizeof(data), &event)
          && event.kind == FETCH_WEBSOCKET_EVENT_OPEN);
    CHECK(fetch_background_websocket_send(
        second, (const unsigned char *) hello, sizeof(hello) - 1u, false));
    CHECK(fetch_background_websocket_close(second, 1000u, "done"));
    bool closed = false;
    for (unsigned events = 0; events < 4u && !closed; events++) {
        if (!take_event(second, data, sizeof(data), &event)) break;
        closed = event.kind == FETCH_WEBSOCKET_EVENT_CLOSE;
    }
    CHECK(closed && event.was_clean && event.close_code == 1000u
          && strcmp(event.reason, "done") == 0);

    pthread_join(thread, NULL);
    CHECK(atomic_load(&server.failed) == 0);
    CHECK(atomic_load(&server.page_close_code) == 1000);

    /* 5. Both sockets are reaped and every buffer returned. */
    CHECK(socket == NULL
          || atomic_load(&socket->state) == FETCH_BACKGROUND_WEBSOCKET_FREE);
    CHECK(socket == NULL || socket->easy == NULL);
    CHECK(test_budget_outstanding == 0);

    close(server.listener);
    curl_multi_cleanup(fetch_background.multi);
    curl_share_cleanup(test_share);
    curl_global_cleanup();
    if (failures != 0) {
        fprintf(stderr, "websocket-loopback: %d failure(s)\n", failures);
        return 1;
    }
    printf("websocket-loopback: open, send, receive and close passed\n");
    return 0;
}
