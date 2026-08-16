#include "tilefinch/fetch.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/content_security_policy.h"
#include "tilefinch/fetch_fault.h"
#include "tilefinch/platform.h"
#include "tilefinch/psp_threads.h"
#include "tilefinch/session.h"
#include "tilefinch/tls_session_store.h"
#include "tilefinch/url.h"

#include <curl/curl.h>

#include "fetch_redirect_abort.h"

/* The explicit legacy PSP escape hatch still ships libcurl 7.64. Map the
   post-7.85 string-list protocol options and 7.66's curl_multi_poll onto
   their older equivalents without weakening the project-owned default. */
#if LIBCURL_VERSION_NUM < 0x075500 /* 7.85.0 */
#define TILEFINCH_CURL_PROTOCOLS_OPT CURLOPT_PROTOCOLS
#define TILEFINCH_CURL_REDIR_PROTOCOLS_OPT CURLOPT_REDIR_PROTOCOLS
#define TILEFINCH_CURL_HTTP_PROTOCOLS \
    ((long) (CURLPROTO_HTTP | CURLPROTO_HTTPS))
#else
#define TILEFINCH_CURL_PROTOCOLS_OPT CURLOPT_PROTOCOLS_STR
#define TILEFINCH_CURL_REDIR_PROTOCOLS_OPT CURLOPT_REDIR_PROTOCOLS_STR
#define TILEFINCH_CURL_HTTP_PROTOCOLS "http,https"
#endif
#if LIBCURL_VERSION_NUM < 0x074200 /* 7.66.0 */
#define curl_multi_poll(multi, extra, count, timeout, numfds) \
    curl_multi_wait((multi), (extra), (count), (timeout), (numfds))
#endif
/* CURLOPT_CAINFO_BLOB arrived in 7.77.0. The owned PSP transport (8.21) and
   modern host libcurl have it and want it (see the fetch_ca_blob_* note); the
   legacy PSP escape hatch still on 7.64 falls back to a CAINFO path. */
#if LIBCURL_VERSION_NUM >= 0x074D00 /* 7.77.0 */
#define TILEFINCH_FETCH_CA_BLOB 1
#endif

#if defined(TILEFINCH_CURL_IMPERSONATE)
extern CURLcode curl_easy_impersonate(CURL *handle, const char *target,
                                      int default_headers);
#endif
#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)
/* The project-owned PSP transport links Mbed TLS directly, so the negotiated
   protocol version and the ClientHello preference lists are reachable through
   libcurl's documented mbedTLS seams without patching either dependency. */
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>

#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)
#include <pspkernel.h>

#include "tilefinch/psp_log.h"
#include "psp_thread_contract.h"
#endif

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_RESOURCE, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_RESOURCE, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_RESOURCE, (p), (s))

#define FETCH_EXTRA_HEADERS_MAX 8192
#define FETCH_EXTRA_HEADER_LINE_MAX 4096
#define FETCH_REQUEST_BODY_MAX (64u * 1024u)
#define FETCH_TRACE_PATH_CAPACITY 4096
/* The pool is process-global because libcurl's allocator hooks are global.
   A streaming navigation and the document resource scheduler deliberately
   overlap, so sizing this for only one four-request scheduler causes valid
   transfers to fail inside nghttp2 before the browser budget is pressured.
   One MiB covers the bounded navigation + resource overlap while remaining
   a fixed, fully accounted reservation on the 32 MiB target. */
#define FETCH_CURL_CONCURRENT_POOL_BYTES (1024u * 1024u)

typedef struct {
    bool required;
    bool blocked;
} FetchPrivateNetworkGuard;

static bool fetch_private_ipv4(const unsigned char address[4])
{
    return address[0] == 0u || address[0] == 10u || address[0] == 127u
        || (address[0] == 100u && (address[1] & 0xc0u) == 0x40u)
        || (address[0] == 169u && address[1] == 254u)
        || (address[0] == 172u && address[1] >= 16u && address[1] <= 31u)
        || (address[0] == 192u && address[1] == 168u)
        || (address[0] == 198u && (address[1] & 0xfeu) == 18u);
}

static bool fetch_private_ipv6(const unsigned char address[16])
{
    static const unsigned char unspecified[16] = {0};
    static const unsigned char loopback[16] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
    };
    static const unsigned char mapped_prefix[12] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff
    };
    return memcmp(address, unspecified, sizeof(unspecified)) == 0
        || memcmp(address, loopback, sizeof(loopback)) == 0
        || (address[0] & 0xfeu) == 0xfcu
        || (address[0] == 0xfeu && (address[1] & 0xc0u) == 0x80u)
        || (memcmp(address, mapped_prefix, sizeof(mapped_prefix)) == 0
            && fetch_private_ipv4(address + sizeof(mapped_prefix)));
}

static bool fetch_private_sockaddr(const struct sockaddr *address)
{
    if (address == NULL) return true;
    if (address->sa_family == AF_INET) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *) address;
        return fetch_private_ipv4((const unsigned char *) &ipv4->sin_addr);
    }
#ifdef AF_INET6
    if (address->sa_family == AF_INET6) {
#if defined(__PSP__)
        /* PSPSDK exposes AF_INET6 but no sockaddr_in6, and the supported PSP
           transport cannot connect IPv6. Treat any such future callback as
           private/blocked rather than guessing at an unavailable layout. */
        return true;
#else
        const struct sockaddr_in6 *ipv6 =
            (const struct sockaddr_in6 *) address;
        return fetch_private_ipv6(
            (const unsigned char *) &ipv6->sin6_addr);
#endif
    }
#endif
    return true;
}


/* The fetch implementation is compiled as one private unit because libcurl
   callbacks and trace replay share bounded state.  Responsibility-specific
   seams keep that state reviewable without exporting it. */
#include "fetch/policy.inc"
#include "fetch/request_authority.inc"
#include "tilefinch/user_agent.h"
static const char *fetch_effective_user_agent(const FetchRequest *request,
                                               bool mobile_safari)
{
    if (request != NULL && request->user_agent != NULL) {
        return request->user_agent;
    }
    return mobile_safari
        ? "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.4 Mobile/15E148 Safari/604.1"
        : TILEFINCH_BROWSER_USER_AGENT;
}

static const char *fetch_accept_encoding(bool mobile_safari)
{
    /* An empty value asks libcurl to advertise every encoding it can decode.
       The diagnostic Safari profile uses an explicit browser-like inventory,
       but it must still be intersected with this libcurl build's decoders. */
    if (!mobile_safari) return "";
    const curl_version_info_data *version =
        curl_version_info(CURLVERSION_NOW);
    long features = version == NULL ? 0 : version->features;
    bool deflate = (features & CURL_VERSION_LIBZ) != 0;
    bool brotli = false;
#if defined(CURL_VERSION_BROTLI)
    brotli = (features & CURL_VERSION_BROTLI) != 0;
#endif
    if (deflate) {
        return brotli ? "gzip, deflate, br" : "gzip, deflate";
    }
    return brotli ? "br" : "identity";
}

static bool append_extra_headers(Budget *budget, struct curl_slist **headers,
                                 const char *serialized)
{
    if (serialized == NULL || serialized[0] == '\0') return true;
    if (!serialized_headers_valid(serialized)) return false;
    char *line = budget_malloc(budget, FETCH_EXTRA_HEADER_LINE_MAX);
    if (line == NULL) return false;
    bool ok = true;
    const char *at = serialized;
    while (*at != '\0') {
        const char *end = strchr(at, '\n');
        size_t line_length = end == NULL ? strlen(at) : (size_t) (end - at);
        if (line_length == 0 || line_length >= FETCH_EXTRA_HEADER_LINE_MAX
            || memchr(at, '\r', line_length) != NULL) { ok = false; break; }
        const char *colon = memchr(at, ':', line_length);
        if (colon == NULL || colon == at) { ok = false; break; }
        for (const char *name = at; name < colon; name++) {
            if (!header_name_character((unsigned char) *name)) {
                ok = false;
                break;
            }
        }
        if (!ok) break;
        for (const char *value = colon + 1; value < at + line_length; value++) {
            unsigned char byte = (unsigned char) *value;
            if (byte < 0x20 || byte == 0x7f) {
                ok = false;
                break;
            }
        }
        if (!ok) break;
        memcpy(line, at, line_length);
        line[line_length] = '\0';
        struct curl_slist *next = curl_slist_append(*headers, line);
        if (next == NULL) { ok = false; break; }
        *headers = next;
        if (end == NULL) break;
        at = end + 1;
    }
    budget_free(budget, line);
    return ok;
}

typedef struct FetchHopState FetchHopState;

static const char *fetch_hop_logical_url(const FetchHopState *hop);
static const char *fetch_hop_wire_url(const FetchHopState *hop);
static bool referrer_policy_known(const char *value, size_t length);
static bool referrer_policy_normalized(const char *value);

typedef struct {
    FetchResult *result;
    size_t maximum_bytes;
    bool limit_exceeded;
    bool allocation_failed;
    FetchCancelCallback cancel;
    void *cancel_opaque;
    bool cancelled;
    const FetchStreamOptions *stream;
    FetchStreamMetrics *metrics;
    size_t delivered;
    bool consumer_failed;
    bool headers_delivered;
    CURL *easy;
    FILE *capture_body;
    char *capture_body_path;
    size_t capture_sequence;
    bool has_capture_sequence;
    bool capture_failed;
    bool cookie_header_rejected;
    bool client_hint_header_rejected;
    bool redirect_callback_aborted;
    size_t response_cookie_start;
    bool has_response_location;
    bool redirect_response;
    /* Host-only trace acquisition returns a redirect as the terminal
       response. Zero preserves normal browser redirect following. */
    bool single_hop;
    bool interim_response;
    FetchCredentialPolicy credentials;
    const char *credential_origin;
    bool credentials_tainted;
    FetchHopState *hop;
    bool cors_rejected;
    bool diagnostic_mobile_safari;
    /* Scheduler-only cooperative delivery state.  Synchronous fetches leave
       pump_quota_enabled false and retain their historical behavior. */
    bool pump_quota_enabled;
    size_t *pump_bytes_remaining;
    size_t *pump_callbacks_remaining;
    FetchPumpMetrics *pump_metrics;
    unsigned char *pending;
    size_t pending_offset;
    size_t pending_length;
    size_t accepted;
    bool defer_replay_body;
    FILE *deferred_replay_body;
    size_t deferred_replay_length;
    uint64_t replay_body_hash;
    uint64_t replay_expected_body_hash;
    bool replay_body_hash_required;
} WriteContext;

static size_t receive_data(char *data, size_t size, size_t count,
                           void *opaque);

static Budget *curl_budget;
static size_t curl_global_users;
/* Fetch schedulers are cooperatively pumped on the browser thread. Sharing
   DNS and TLS-session caches therefore needs no cross-thread callbacks, and
   avoids repeating expensive handheld DNS, TLS-session, and reusable
   connection setup when a document discovers same-origin resources. */
static CURLSH *curl_shared_state;
static BudgetConcurrentPool curl_concurrent_pool;
static BudgetConcurrentPoolMetrics curl_last_pool_metrics;
static char curl_global_error[96];
static char fetch_ca_bundle[1024];
/*
 * The trust bundle is configured as a path but installed as an in-memory blob
 * (CURLOPT_CAINFO_BLOB). A path-based CAINFO gives curl's session cache a
 * peer key that both realpath()-fails on ms0: (making every peer :L, which
 * curl refuses to export) and embeds the active program slot (which would
 * orphan the cross-boot store on every A/B update). Hashing the blob content
 * makes the key exportable (:G) and slot-independent. The bytes are read once
 * on first use and cached until the configured path changes; plain malloc
 * keeps them off the page budget, which frees before quiesce.
 */
#ifdef TILEFINCH_FETCH_CA_BLOB
static unsigned char *fetch_ca_blob_data;
static size_t fetch_ca_blob_length;
static char fetch_ca_blob_source[sizeof(fetch_ca_bundle)];
#endif
/* Cross-boot TLS session resumption store. Empty disables it. */
static char fetch_tls_session_store_file[1024];
static atomic_bool fetch_tls_session_persistence = ATOMIC_VAR_INIT(true);
#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)
static TlsSessionStore *fetch_tls_session_memory;
static bool fetch_tls_session_load_attempted;
static bool fetch_tls_session_dirty;
static void fetch_tls_session_memory_reset(void);
#endif

static void fetch_ca_blob_reset(void)
{
#ifdef TILEFINCH_FETCH_CA_BLOB
    free(fetch_ca_blob_data);
    fetch_ca_blob_data = NULL;
    fetch_ca_blob_length = 0;
    fetch_ca_blob_source[0] = '\0';
#endif
}

bool fetch_set_ca_bundle_path(const char *path)
{
    if (curl_global_users != 0) return false;
    if (path == NULL || path[0] == '\0') {
        fetch_ca_bundle[0] = '\0';
        fetch_ca_blob_reset();
        return true;
    }
    size_t length = strlen(path);
    if (length >= sizeof(fetch_ca_bundle)
        || memchr(path, '\r', length) != NULL
        || memchr(path, '\n', length) != NULL) return false;
    memcpy(fetch_ca_bundle, path, length + 1);
    /* Invalidate any blob cached from a previous path; it is re-read lazily. */
    fetch_ca_blob_reset();
    return true;
}

const char *fetch_ca_bundle_path(void)
{
    return fetch_ca_bundle[0] == '\0' ? NULL : fetch_ca_bundle;
}

#ifdef TILEFINCH_FETCH_CA_BLOB
/* Reads the configured trust bundle into fetch_ca_blob_data once, caching it
   until the path changes. Returns true when a blob is available. */
static bool fetch_ca_blob_ensure(void)
{
    if (fetch_ca_bundle[0] == '\0') return false;
    if (fetch_ca_blob_data != NULL
        && strcmp(fetch_ca_blob_source, fetch_ca_bundle) == 0) {
        return true;
    }
    fetch_ca_blob_reset();
    FILE *file = fopen(fetch_ca_bundle, "rb");
    if (file == NULL) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long size = ftell(file);
    if (size <= 0 || size > 512L * 1024L || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    unsigned char *data = malloc((size_t) size);
    if (data == NULL) { fclose(file); return false; }
    bool ok = fread(data, 1, (size_t) size, file) == (size_t) size;
    fclose(file);
    if (!ok) { free(data); return false; }
    fetch_ca_blob_data = data;
    fetch_ca_blob_length = (size_t) size;
    snprintf(fetch_ca_blob_source, sizeof(fetch_ca_blob_source), "%s",
             fetch_ca_bundle);
    return true;
}
#endif /* TILEFINCH_FETCH_CA_BLOB */

bool fetch_set_tls_session_store_path(const char *path)
{
    if (curl_global_users != 0) return false;
    const char *next = path == NULL ? "" : path;
    if (strcmp(fetch_tls_session_store_file, next) == 0) return true;
#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)
    if (!fetch_tls_session_store_flush()) return false;
    fetch_tls_session_memory_reset();
#endif
    if (path == NULL || path[0] == '\0') {
        fetch_tls_session_store_file[0] = '\0';
        return true;
    }
    size_t length = strlen(path);
    if (length >= sizeof(fetch_tls_session_store_file)) return false;
    memcpy(fetch_tls_session_store_file, path, length + 1);
    return true;
}

const char *fetch_tls_session_store_path(void)
{
    return fetch_tls_session_store_file[0] == '\0'
        ? NULL : fetch_tls_session_store_file;
}

bool fetch_set_tls_session_persistence_enabled(bool enabled)
{
    atomic_store_explicit(
        &fetch_tls_session_persistence, enabled, memory_order_release);
#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)
    /* Do not reset process-resident state here: the transport worker can be
       inside import/export while Options is handled on the browser thread.
       Suppressing future import/export/flush is enough, and removing the
       durable generation does not touch worker-owned memory. */
    if (!enabled && fetch_tls_session_store_file[0] != '\0')
        return tls_session_store_remove(fetch_tls_session_store_file) == 0;
#else
    (void) enabled;
#endif
    return true;
}

bool fetch_tls_session_persistence_enabled(void)
{
    return atomic_load_explicit(
        &fetch_tls_session_persistence, memory_order_acquire);
}

const char *fetch_transport_version(void)
{
    curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    return info == NULL || info->version == NULL
        ? "unavailable" : info->version;
}

const char *fetch_transport_tls_version(void)
{
    curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    return info == NULL || info->ssl_version == NULL
        ? "unavailable" : info->ssl_version;
}

const char *fetch_transport_http2_version(void)
{
    curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
#if LIBCURL_VERSION_NUM >= 0x072B00 /* 7.43.0 */
    return info == NULL || info->nghttp2_version == NULL
        ? "disabled" : info->nghttp2_version;
#else
    (void) info;
    return "disabled";
#endif
}

bool fetch_transport_http2_available(void)
{
    curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
#if defined(CURL_VERSION_HTTP2)
    return info != NULL && (info->features & CURL_VERSION_HTTP2) != 0;
#else
    (void) info;
    return false;
#endif
}

const char *fetch_http_version_name(long version)
{
    switch (version) {
#if defined(CURL_HTTP_VERSION_1_0)
        case CURL_HTTP_VERSION_1_0: return "1.0";
#endif
#if defined(CURL_HTTP_VERSION_1_1)
        case CURL_HTTP_VERSION_1_1: return "1.1";
#endif
#if defined(CURL_HTTP_VERSION_2_0)
        case CURL_HTTP_VERSION_2_0: return "2";
#endif
        default: return "unknown";
    }
}

static bool fetch_transport_runtime_supported(void)
{
#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)
    curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    if (info == NULL || info->version == NULL || info->ssl_version == NULL
        || strcmp(info->version, TILEFINCH_PSP_CURL_VERSION) != 0
        || strncmp(info->ssl_version, "mbedTLS/", 8) != 0
        || strcmp(info->ssl_version + 8,
                  TILEFINCH_PSP_MBEDTLS_VERSION) != 0) {
        return false;
    }
#if defined(TILEFINCH_PSP_HTTP2)
    if (!fetch_transport_http2_available()
        || info->nghttp2_version == NULL
        || strcmp(info->nghttp2_version,
                  TILEFINCH_PSP_NGHTTP2_VERSION) != 0) {
        return false;
    }
#else
    if (fetch_transport_http2_available()) return false;
#endif
#endif
    return true;
}

static bool fetch_configure_http_version(CURL *easy)
{
#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)
#if defined(TILEFINCH_PSP_HTTP2)
    return curl_easy_setopt(
               easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS) == CURLE_OK;
#else
    return curl_easy_setopt(
               easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1) == CURLE_OK;
#endif
#else
    (void) easy;
    return true;
#endif
}

/*
 * Handshake attribution (docs/engineering/PSP_TRANSPORT.md).
 *
 * libcurl's coarse handshake number is APPCONNECT minus CONNECT: the wall
 * time between the TCP connection being usable and the TLS session being
 * usable.  Both timers are per-transfer, so a handle that was handed a
 * pooled connection reports zero for each of them and there is no handshake
 * to attribute.  That is the reuse signal we trust here; CURLINFO_NUM_CONNECTS
 * counts new connections but cannot tell a plain-HTTP connect from a TLS one.
 *
 * The vendored curl 8.21 mbedTLS backend does not expose the handshake's
 * internal split (key exchange vs chain verify) to callers: its vtls layer
 * neither times those phases nor surfaces an mbedTLS hook that would.  Only
 * the coarse number is recorded here; a finer split would need a curl patch,
 * which this phase deliberately does not take.
 */
typedef struct {
    bool classified;
    bool measured;
    uint64_t handshake_us;
    bool reused;
} FetchHandshakeSample;

static bool fetch_transport_hop_is_tls(CURL *easy)
{
    char *url = NULL;
    return curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &url) == CURLE_OK
        && url != NULL && strncasecmp(url, "https://", 8) == 0;
}

/* Samples the completed hop on `easy`.  Only call this for a hop that
   reached the HTTP layer: on a connection that failed to establish, the
   zeroed timers would otherwise be indistinguishable from reuse. */
static void fetch_handshake_sample(CURL *easy, FetchHandshakeSample *sample)
{
    memset(sample, 0, sizeof(*sample));
#if LIBCURL_VERSION_NUM >= 0x073D00 /* 7.61.0 introduced the _TIME_T infos */
    if (easy == NULL || !fetch_transport_hop_is_tls(easy)) return;
    curl_off_t connect_us = 0;
    curl_off_t appconnect_us = 0;
    if (curl_easy_getinfo(
            easy, CURLINFO_CONNECT_TIME_T, &connect_us) != CURLE_OK
        || curl_easy_getinfo(
               easy, CURLINFO_APPCONNECT_TIME_T, &appconnect_us) != CURLE_OK) {
        return;
    }
    sample->classified = true;
    sample->reused = appconnect_us <= 0;
    if (!sample->reused && appconnect_us > connect_us) {
        sample->measured = true;
        sample->handshake_us = (uint64_t) (appconnect_us - connect_us);
    }
#else
    (void) easy;
#endif
}

static void fetch_tls_counters_add(FetchTlsHandshakeCounters *counters,
                                   const FetchHandshakeSample *sample)
{
    if (counters == NULL || !sample->classified) return;
    counters->transfers++;
    if (sample->reused) counters->reused_connections++;
    else counters->full_handshakes++;
    if (!sample->measured) return;
    counters->measured++;
    counters->total_handshake_us += sample->handshake_us;
    if (sample->handshake_us > counters->maximum_handshake_us) {
        counters->maximum_handshake_us = sample->handshake_us;
    }
}

static FetchTlsHandshakeCounters fetch_tls_counters;

/* Records one completed transport hop into the process-wide totals and, when
   the hop belongs to a scheduler, that scheduler's own view. */
static void fetch_tls_counters_record(CURL *easy,
                                      FetchTlsHandshakeCounters *view)
{
    FetchHandshakeSample sample;
    fetch_handshake_sample(easy, &sample);
    fetch_tls_counters_add(&fetch_tls_counters, &sample);
    fetch_tls_counters_add(view, &sample);
}

void fetch_tls_handshake_counters(FetchTlsHandshakeCounters *counters)
{
    if (counters == NULL) return;
    *counters = fetch_tls_counters;
}

void fetch_tls_handshake_counters_reset(void)
{
    memset(&fetch_tls_counters, 0, sizeof(fetch_tls_counters));
}

bool fetch_tls_handshake_counters_format(
    const FetchTlsHandshakeCounters *counters, char *output,
    size_t output_size)
{
    if (counters == NULL || output == NULL || output_size == 0) return false;
    output[0] = '\0';
    char total[32] = "absent";
    char maximum[32] = "absent";
    if (counters->measured != 0) {
        (void) snprintf(total, sizeof(total), "%llu",
                        (unsigned long long) counters->total_handshake_us);
        (void) snprintf(maximum, sizeof(maximum), "%llu",
                        (unsigned long long) counters->maximum_handshake_us);
    }
    int written = snprintf(
        output, output_size,
        "transfers=%zu full=%zu reused=%zu measured=%zu total-us=%s max-us=%s",
        counters->transfers, counters->full_handshakes,
        counters->reused_connections, counters->measured, total, maximum);
    if (written < 0 || (size_t) written >= output_size) {
        output[0] = '\0';
        return false;
    }
    return true;
}

/* PSP-only: the vendored curl mbedTLS backend hands callers the live
   mbedtls_ssl_context through CURLINFO_TLS_SSL_PTR, which is the one place
   the negotiated protocol version is readable without patching curl.  It is
   valid only while the transfer owns its connection, so it is sampled from
   the response status line rather than after the transfer completes.  The
   host OpenSSL path leaves tls_version empty. */
static void fetch_result_capture_tls_version(CURL *easy, FetchResult *result)
{
#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)
    if (easy == NULL || result == NULL || result->tls_version[0] != '\0') {
        return;
    }
    struct curl_tlssessioninfo *session = NULL;
    if (curl_easy_getinfo(easy, CURLINFO_TLS_SSL_PTR, &session) != CURLE_OK
        || session == NULL || session->internals == NULL
        || session->backend != CURLSSLBACKEND_MBEDTLS) return;
    const char *version = mbedtls_ssl_get_version(
        (const mbedtls_ssl_context *) session->internals);
    if (version == NULL || version[0] == '\0'
        || strcmp(version, "unknown") == 0) return;
    (void) snprintf(result->tls_version, sizeof(result->tls_version), "%s",
                    version);
#else
    (void) easy;
    (void) result;
#endif
}

static void fetch_result_set_transport_info(CURL *easy, FetchResult *result)
{
    if (easy == NULL || result == NULL) return;
    (void) curl_easy_getinfo(
        easy, CURLINFO_SSL_VERIFYRESULT, &result->tls_verify_result);
    (void) curl_easy_getinfo(
        easy, CURLINFO_HTTP_VERSION, &result->negotiated_http_version);
#if LIBCURL_VERSION_NUM >= 0x073200 /* 7.50.0 */
    (void) curl_easy_getinfo(
        easy, CURLINFO_NUM_CONNECTS, &result->new_connections);
#endif
    FetchHandshakeSample sample;
    fetch_handshake_sample(easy, &sample);
    /* Assign unconditionally: a handle reused across redirect hops must not
       leave an earlier hop's handshake attributed to this result. */
    result->tls_connection_reuse_known = sample.classified;
    result->tls_connection_reused = sample.classified && sample.reused;
    result->tls_handshake_measured = sample.measured;
    result->tls_handshake_us = sample.measured ? sample.handshake_us : 0;
}

static void fetch_result_log_transport(const FetchResult *result)
{
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    if (result == NULL) return;
    char handshake[32] = "absent";
    if (result->tls_handshake_measured) {
        (void) snprintf(handshake, sizeof(handshake), "%llu",
                        (unsigned long long) result->tls_handshake_us);
    }
    /* Do not log the URL: media URLs and ordinary navigation URLs can contain
       credentials or durable query tokens.  These fields are sufficient to
       diagnose protocol selection and connection reuse on device. */
    fprintf(stdout,
            "tilefinch-transport-result: http=%s new-connections=%ld "
            "status=%ld bytes=%zu tls=%s tls-handshake-us=%s "
            "tls-version=%s tls12-retry=%d\n",
            fetch_http_version_name(result->negotiated_http_version),
            result->new_connections, result->status_code,
            result->received_body_bytes,
            result->tls_connection_reuse_known
                ? (result->tls_connection_reused ? "reused" : "full")
                : "absent",
            handshake,
            result->tls_version[0] == '\0' ? "absent" : result->tls_version,
            result->tls12_compatibility_retry ? 1 : 0);
    char counters[128];
    FetchTlsHandshakeCounters totals;
    fetch_tls_handshake_counters(&totals);
    if (fetch_tls_handshake_counters_format(
            &totals, counters, sizeof(counters))) {
        fprintf(stdout, "tilefinch-tls-handshake: %s\n", counters);
    }
#else
    (void) result;
#endif
}

#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)
/*
 * ClientHello shaping (docs/engineering/PSP_TRANSPORT.md).
 *
 * Both lists below are preference orders only.  Nothing is removed from what
 * the ClientHello offers: Mbed TLS drops entries this build cannot support
 * when it writes the supported_groups and signature_algorithms extensions, so
 * a superset list advertises exactly the same set as the library default and
 * only changes the order.  Certificate validation, the verification profile,
 * and the ciphersuite floor are untouched.
 *
 * The vendored curl 8.21 mbedTLS backend ignores CURLOPT_SSL_EC_CURVES (its
 * vtls layer never reads ssl_ec_curves and never calls
 * mbedtls_ssl_conf_groups), but it does invoke CURLOPT_SSL_CTX_FUNCTION with
 * the live mbedtls_ssl_config, which is the documented seam for exactly this.
 */
static const uint16_t fetch_tls_group_preference[] = {
    /* Curve25519 first: it is several times cheaper than P-256 in portable C,
       and Mbed TLS derives the TLS 1.3 key_share from the first supported
       entry, so this also avoids a HelloRetryRequest round-trip. */
    MBEDTLS_SSL_IANA_TLS_GROUP_X25519,
    MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,
    MBEDTLS_SSL_IANA_TLS_GROUP_SECP384R1,
    MBEDTLS_SSL_IANA_TLS_GROUP_X448,
    MBEDTLS_SSL_IANA_TLS_GROUP_SECP521R1,
    MBEDTLS_SSL_IANA_TLS_GROUP_BP256R1,
    MBEDTLS_SSL_IANA_TLS_GROUP_BP384R1,
    MBEDTLS_SSL_IANA_TLS_GROUP_BP512R1,
    /* Finite-field groups stay offered so the ciphersuite floor keeps every
       DHE key exchange it already had. */
    MBEDTLS_SSL_IANA_TLS_GROUP_FFDHE2048,
    MBEDTLS_SSL_IANA_TLS_GROUP_FFDHE3072,
    MBEDTLS_SSL_IANA_TLS_GROUP_FFDHE4096,
    MBEDTLS_SSL_IANA_TLS_GROUP_FFDHE6144,
    MBEDTLS_SSL_IANA_TLS_GROUP_FFDHE8192,
    MBEDTLS_SSL_IANA_TLS_GROUP_NONE
};

static const uint16_t fetch_tls_signature_preference[] = {
    /* RSA first so a dual-chain CDN grants the RSA chain, whose verify is
       several times cheaper than ECDSA on this CPU.  SHA-256 leads each
       family for the same reason. */
    MBEDTLS_TLS1_3_SIG_RSA_PSS_RSAE_SHA256,
    MBEDTLS_TLS1_3_SIG_RSA_PSS_RSAE_SHA384,
    MBEDTLS_TLS1_3_SIG_RSA_PSS_RSAE_SHA512,
    MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA256,
    MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA384,
    MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA512,
    /* ECDSA remains advertised: single-chain ECDSA hosts must keep working,
       and dropping it would be a compatibility regression, not a speedup. */
    MBEDTLS_TLS1_3_SIG_ECDSA_SECP256R1_SHA256,
    MBEDTLS_TLS1_3_SIG_ECDSA_SECP384R1_SHA384,
    MBEDTLS_TLS1_3_SIG_ECDSA_SECP521R1_SHA512,
    MBEDTLS_TLS1_3_SIG_NONE
};

static CURLcode fetch_tls_shape_client_hello(CURL *easy, void *ssl_config,
                                             void *opaque)
{
    (void) easy;
    (void) opaque;
    if (ssl_config == NULL) return CURLE_SSL_CONNECT_ERROR;
    /* Neither call copies its array; both lists have static storage. */
    mbedtls_ssl_conf_groups((mbedtls_ssl_config *) ssl_config,
                            fetch_tls_group_preference);
#if defined(MBEDTLS_SSL_HANDSHAKE_WITH_CERT_ENABLED)
    mbedtls_ssl_conf_sig_algs((mbedtls_ssl_config *) ssl_config,
                              fetch_tls_signature_preference);
#endif
    return CURLE_OK;
}
#endif /* TILEFINCH_PSP_OWNED_TRANSPORT */

static bool fetch_configure_tls(CURL *easy)
{
    if (easy == NULL
        || curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L) != CURLE_OK) {
        return false;
    }
#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)
    if (curl_easy_setopt(easy, CURLOPT_SSL_CTX_FUNCTION,
                         fetch_tls_shape_client_hello) != CURLE_OK) {
        return false;
    }
#endif
    if (fetch_ca_bundle[0] == '\0') return true;
    /* A configured CA file does not replace libcurl's compiled-in CA
       directory (PSPDEV's curl names /etc/ssl/certs, absent on the PSP, which
       mbedTLS treats as fatal even after our bundle reads), so CAPATH is
       cleared so roots.pem is the single verified trust source. */
    if (curl_easy_setopt(easy, CURLOPT_CAPATH, NULL) != CURLE_OK) return false;
#ifdef TILEFINCH_FETCH_CA_BLOB
    /* Install the bundle as an in-memory blob rather than a path (see the
       fetch_ca_blob_* note): the path-based CAINFO key both realpath()-fails
       on ms0: (barring session export) and embeds the program slot. Failing
       to read the configured bundle fails closed, exactly as a missing CAINFO
       file did. */
    if (!fetch_ca_blob_ensure()) return false;
    struct curl_blob ca_blob = {
        .data = fetch_ca_blob_data,
        .len = fetch_ca_blob_length,
        .flags = CURL_BLOB_NOCOPY
    };
    return curl_easy_setopt(easy, CURLOPT_CAINFO_BLOB, &ca_blob) == CURLE_OK;
#else
    return curl_easy_setopt(easy, CURLOPT_CAINFO, fetch_ca_bundle) == CURLE_OK;
#endif
}

/*
 * The connect phase -- DNS, TCP, TLS -- is the part of a request that reports
 * no progress, so the cancellation callback (which libcurl invokes from the
 * progress callback) cannot run inside it. Its length is therefore the length
 * of a blind window on whichever thread called fetch. Historically that window
 * was half the request deadline, which is 7.5 s for a media range read and
 * 15 s for a resolve. FetchRequest::connect_timeout_ms lets a caller that runs
 * fetch on the interactive thread shorten it. It may only tighten: a caller
 * cannot use it to sit in connect for longer than its own deadline allows.
 */
static long fetch_connect_timeout_ms(
    const FetchRequest *request, long deadline_ms)
{
    long connect_ms = deadline_ms / 2;
    if (connect_ms <= 0) connect_ms = 1;
    if (request != NULL && request->connect_timeout_ms > 0
        && request->connect_timeout_ms < connect_ms) {
        connect_ms = request->connect_timeout_ms;
    }
    return connect_ms;
}

/*
 * A transfer that is connected but receiving nothing is the other way a
 * request spends seconds without giving the cancellation callback anything to
 * act on: libcurl keeps waiting on a socket that never becomes readable until
 * the whole request deadline expires. CURLOPT_LOW_SPEED_LIMIT/_TIME is
 * enforced inside libcurl's own wait loop (progress.c pgrs_speedcheck, armed
 * by an EXPIRE_SPEEDCHECK timer that fires every second even with no socket
 * activity), so it needs no progress callback of ours and no polling.
 *
 * The threshold is deliberately at the floor: fewer than one byte per second
 * averaged over three seconds is a dead transfer, not a slow one, so this is
 * safe to apply to every blocking request rather than only to media. A page
 * load gains the same thing video does -- a stalled response fails in three
 * seconds instead of at the deadline -- and no genuinely progressing PSP Wi-Fi
 * transfer, however slow, can trip it. The scheduler's asynchronous
 * subresource path is deliberately left alone: it does not block a thread, so
 * a stall there costs no responsiveness.
 */
#define FETCH_STALL_LOW_SPEED_BYTES_PER_SECOND 1L
#define FETCH_STALL_LOW_SPEED_SECONDS 3L

static bool fetch_configure_stall_watchdog(CURL *easy)
{
    return curl_easy_setopt(
               easy, CURLOPT_LOW_SPEED_LIMIT,
               FETCH_STALL_LOW_SPEED_BYTES_PER_SECOND) == CURLE_OK
        && curl_easy_setopt(
               easy, CURLOPT_LOW_SPEED_TIME,
               FETCH_STALL_LOW_SPEED_SECONDS) == CURLE_OK;
}

/*
 * Some production endpoints exposed a real PSP interoperability boundary in
 * the Mbed TLS 3.6 TLS 1.3 handshake: the peer returns illegal_parameter before
 * any HTTP bytes. Orion's proven PSP transport uses verified TLS 1.2, and an
 * owned-transport PPSSPP probe confirmed the same request succeeds there.
 *
 * Keep TLS 1.3 as the default path. A handshake failure may retry once, only
 * before headers or body exist, inside the original request deadline. This
 * is intentionally unavailable to host and legacy transports, whose TLS
 * behavior is different and supplied no evidence for this workaround.
 */
static bool fetch_try_tls12_compatibility(
    CURL *easy, CURLcode result, bool *attempted, long remaining_ms,
    const FetchRequest *request)
{
#if defined(TILEFINCH_PSP_OWNED_TRANSPORT)
    if (easy == NULL || attempted == NULL || *attempted
        || result != CURLE_SSL_CONNECT_ERROR || remaining_ms <= 0) {
        return false;
    }
    *attempted = true;
    long connect_ms = fetch_connect_timeout_ms(request, remaining_ms);
    return curl_easy_setopt(
               easy, CURLOPT_SSLVERSION,
               CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2)
               == CURLE_OK
        && curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, remaining_ms)
               == CURLE_OK
        && curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, connect_ms)
               == CURLE_OK;
#else
    (void) easy;
    (void) result;
    (void) attempted;
    (void) remaining_ms;
    (void) request;
    return false;
#endif
}

typedef enum {
    FETCH_TRACE_OFF,
    FETCH_TRACE_CAPTURE_ARMED,
    FETCH_TRACE_CAPTURE,
    FETCH_TRACE_REPLAY
} FetchTraceMode;

/* Capture/replay is a lab feature, but it still needs a finite authority
   surface.  Four thousand records is comfortably above the retained corpora
   while keeping the claimed-record set allocation-free and deterministic. */
#define FETCH_TRACE_RECORD_LIMIT 4096u
#define FETCH_TRACE_ASYNC_DELAY_PUMP_LIMIT 1000000u
#define FETCH_TRACE_ROUTE_INDEX_LIMIT 512u
#define FETCH_TRACE_CLAIM_WORDS \
    ((FETCH_TRACE_RECORD_LIMIT + 63u) / 64u)

typedef struct {
    uint64_t key_hash;
    signed char rank;
    bool key_valid;
    bool descriptor_valid;
} TraceReplayRouteIndexEntry;

static struct {
    FetchTraceMode mode;
    char directory[2048];
    size_t sequence;
    uint64_t clock_origin_ms;
    bool capture_raw_cookie_values;
    size_t top_level_request_ordinal;
    size_t top_level_requests_seen;
    bool replay_starts_at_delayed_top_level;
    size_t replay_record_count;
    bool replay_record_count_authoritative;
    bool replay_response_keyed;
    size_t replay_request_count;
    size_t replay_matched_request_count;
    size_t replay_served_request_count;
    size_t replay_rejected_request_count;
    size_t replay_unmatched_request_count;
    size_t replay_conflicting_request_count;
    size_t replay_invalid_route_request_count;
    size_t replay_request_shape_mismatch_count;
    size_t replay_occurrence_claim_count;
    size_t replay_reusable_claim_count;
    size_t replay_occurrence_exhausted_count;
    uint64_t replay_claimed[FETCH_TRACE_CLAIM_WORDS];
    /* Response-keyed replay is a lab boundary.  Keep its route accelerator
       proportional to small/medium opened corpora instead of reserving the
       4096-record ceiling in every PSP process.  The accelerator is capped
       at 512 records (about 8 KiB); larger corpora retain the allocation-free
       bounded scan. Selected records are still reparsed by the authoritative
       replay parser. */
    TraceReplayRouteIndexEntry *replay_route_index;
    size_t replay_route_index_count;
} fetch_trace;

#include "fetch/security_metadata.inc"
#include "fetch/trace_capture.inc"
#include "fetch/trace_replay.inc"
#include "fetch/trace_session.inc"
#include "fetch/response_stream.inc"
#include "fetch/transport.inc"
#include "fetch/scheduler.inc"
#include "fetch/background_transport.inc"
#include "fetch/preconnect.inc"
#include "fetch/result.inc"
