#ifndef TILEFINCH_FETCH_H
#define TILEFINCH_FETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/request_context.h"
#include "tilefinch/url.h"

struct BrowserSession;
struct BrowserSharedBody;
struct ContentBlocker;
struct TilefinchContentSecurityPolicy;

#define FETCH_SET_COOKIE_LIMIT 4096
#define FETCH_RESPONSE_COOKIE_CAPACITY 16
/* Current large-site CSP fields exceed 4 KiB by themselves (4,527 bytes was
   measured in the acceptance corpus on 2026-08-01). Keep this fixed and
   modest: it is part of every FetchResult, but must hold one complete security
   field so the fail-closed parser does not reject an otherwise valid response. */
#define FETCH_RESPONSE_HEADERS_LIMIT 8192
#define FETCH_REFERRER_POLICY_LIMIT 40

/* Security headers are interpreted once, while their complete wire values
   are still available.  Consumers must distinguish absence from malformed,
   duplicate, or retention-failed input instead of reparsing the bounded
   page-visible header snapshot. */
typedef enum {
    FETCH_SECURITY_FIELD_ABSENT = 0,
    FETCH_SECURITY_FIELD_VALID,
    FETCH_SECURITY_FIELD_INVALID,
    FETCH_SECURITY_FIELD_DUPLICATE,
    FETCH_SECURITY_FIELD_TRUNCATED
} FetchSecurityFieldState;

typedef struct FetchResponseSecurityMetadata {
    uint32_t version;
    FetchSecurityFieldState allow_origin_state;
    FetchSecurityFieldState allow_credentials_state;
    FetchSecurityFieldState corp_state;
    FetchSecurityFieldState nosniff_state;
    FetchSecurityFieldState hsts_state;
    FetchSecurityFieldState referrer_policy_state;
    /* CSP and frame policy remain in the bounded response snapshot for their
       existing parsers, but completeness is typed here so loss is explicit. */
    FetchSecurityFieldState csp_snapshot_state;
    FetchSecurityFieldState frame_policy_snapshot_state;
    TilefinchCrossOriginResourcePolicy corp;
    uint64_t hsts_max_age;
    char allow_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char referrer_policy[FETCH_REFERRER_POLICY_LIMIT];
    bool allow_credentials;
    bool nosniff;
    bool hsts_include_subdomains;
    bool snapshot_truncated;
} FetchResponseSecurityMetadata;

#define FETCH_RESPONSE_SECURITY_METADATA_VERSION 1u

void fetch_response_security_metadata_reset(
    FetchResponseSecurityMetadata *metadata);
/* Collects one already-trimmed wire field. snapshot_stored applies only to
   policies whose existing bounded parser still consumes response_headers. */
bool fetch_response_security_metadata_collect(
    FetchResponseSecurityMetadata *metadata,
    const char *name, size_t name_length,
    const char *value, size_t value_length, bool snapshot_stored);
bool fetch_response_security_metadata_from_snapshot(
    FetchResponseSecurityMetadata *metadata,
    const char *headers, size_t headers_length, bool snapshot_truncated);

typedef struct {
    Budget *budget;
    char *data;
    struct BrowserSharedBody *shared_body;
    size_t length;
    size_t capacity;
    /* Body bytes accepted before the terminal result, retained even when the
       synchronous scheduler helper releases a partial failed body.  Callers
       use this for aggregate transfer budgets rather than mistaking failure
       for zero network cost. */
    size_t received_body_bytes;
    long status_code;
    /* Native transport diagnostics. Zero means replay/synthetic/unknown.
       CURLINFO_HTTP_VERSION's numeric value is intentionally not exposed as
       an ABI promise; fetch_http_version_name() is the stable presentation. */
    long negotiated_http_version;
    long new_connections;
    /* TLS handshake attribution for the final transport hop of this request,
       on the same footing as the two fields above: it describes the transfer
       libcurl actually completed, not the whole redirect chain.  Every field
       is explicitly present-or-absent because a transport that cannot report
       a number must not be mistaken for a measured zero.  Replay, synthetic
       results, and plain-HTTP transfers leave all of them cleared. */
    bool tls_handshake_measured;
    uint64_t tls_handshake_us;
    bool tls_connection_reuse_known;
    bool tls_connection_reused;
    /* The PSP transport normally offers TLS 1.3. Some otherwise valid TLS
       endpoints reject that ClientHello before HTTP with a protocol alert.
       A set field records that this request made the single bounded TLS 1.2
       compatibility retry; certificate and hostname verification remain on. */
    bool tls12_compatibility_retry;
    /* Negotiated protocol name ("TLSv1.2"/"TLSv1.3").  Empty when the linked
       TLS backend does not expose it to libcurl callers. */
    char tls_version[16];
    /* Normalized browser-visible response URL. Unlike the HTTP request/cache
       key, this preserves the logical fragment: fragmentless redirects
       inherit the current fragment and an explicit Location fragment replaces
       it. Fragments are never sent on the wire. */
    char effective_url[2048];
    char content_type[128];
    char etag[192];
    char last_modified[128];
    char cf_mitigated[32];
    char accept_ch[1024];
    char critical_ch[1024];
    char server[64];
    char cf_ray[64];
    char response_headers[FETCH_RESPONSE_HEADERS_LIMIT];
    size_t response_headers_length;
    FetchResponseSecurityMetadata security;
    /* Legacy trace/synthetic-result compatibility. Live transport consumers
       use security; these fields are removed after the trace-vNext cutover. */
    char response_referrer_policy[FETCH_REFERRER_POLICY_LIMIT];
    bool response_referrer_policy_header_present;
    bool response_referrer_policy_metadata_valid;
    bool response_security_headers_truncated;
    /* Response-cookie values are allocated only when received. Keeping just
       bounded ownership pointers here avoids putting a worst-case cookie
       payload in every FetchResult and in scheduler/stack frames. */
    char *set_cookies[FETCH_RESPONSE_COOKIE_CAPACITY];
    /* The response URL which supplied each Set-Cookie field. Redirect hops
       can cross origins, so attributing every cookie to effective_url is both
       incorrect and unsafe. These strings are budget-owned. */
    char *set_cookie_urls[FETCH_RESPONSE_COOKIE_CAPACITY];
    size_t set_cookie_count;
    char error[256];
    bool timed_out;
    /* True when any redirect crossed an origin boundary. The taint is sticky
       even if a later hop returns to the initiator's origin; CORS then uses
       the final wire Origin value "null". */
    bool redirect_origin_tainted;
    /* Internal deterministic replay timing; not exposed to page script. */
    size_t trace_delay_pumps;
    bool trace_external_cancel;
} FetchResult;

typedef enum {
    /* Zero preserves the historical browser transport behavior. */
    FETCH_CREDENTIALS_INCLUDE = 0,
    FETCH_CREDENTIALS_OMIT,
    FETCH_CREDENTIALS_SAME_ORIGIN
} FetchCredentialPolicy;

typedef enum {
    FETCH_REDIRECT_DEFAULT = 0,
    /* Native updater-only policy: HTTPS/443 github.com or a label-boundary
       subdomain of githubusercontent.com. Page requests never select it. */
    FETCH_REDIRECT_GITHUB_RELEASE
} FetchRedirectPolicy;

typedef bool (*FetchRedirectUrlValidator)(const char *url);

typedef struct {
    const char *method;
    const char *body;
    size_t body_length;
    const char *content_type;
    /* Newline-delimited, prevalidated "name: value" application headers. */
    const char *extra_headers;
    /* Browser-generated CORS preflight fields use extra_headers as their
       bounded wire representation, but page-controlled callers may never
       inject Access-Control-Request-* themselves.  This flag opens only the
       narrowly validated OPTIONS preflight shape in fetch_request_validate. */
    bool cors_preflight;
    const char *cookie;
    const char *if_none_match;
    const char *if_modified_since;
    bool allow_http_errors;
    /* High-entropy client hints are an explicitly scoped retry capability.
       client_hint_tokens is the comma-separated Critical-CH token set and
       client_hint_origin is the canonical origin which requested it.  The
       transport suppresses these hints on every cross-origin redirect hop.
       Low-entropy hints remain controlled independently below. */
    bool send_client_hints;
    const char *client_hint_tokens;
    const char *client_hint_origin;
    bool send_low_client_hints;
    const char *referer;
    const char *origin;
    const char *accept;
    const char *sec_fetch_dest;
    const char *sec_fetch_mode;
    const char *sec_fetch_site;
    bool sec_fetch_user;
    bool upgrade_insecure_requests;
    bool identity_encoding;
    const char *user_agent;
    /*
     * Optional per-request connect bound in milliseconds, covering DNS-to-TLS
     * establishment. Zero keeps the transport default of half the request
     * deadline. A positive value may only tighten that default, never loosen
     * it: this is a caller promise about how long a single attempt may sit in
     * a phase that reports no progress, not permission to outlive the
     * deadline. Native media callers set it because their attempts run on the
     * main thread inside a cooperate scope; ordinary page fetches leave it
     * zero and keep the historical behavior.
     */
    long connect_timeout_ms;
    /* Native retry policy only. A stalled media-range retry uses this to
       escape the exact HTTP/2 connection whose throughput floor fired.
       Ordinary page requests leave it false and retain connection reuse. */
    bool force_fresh_connection;
    /* Internal transport snapshot for page-originated PNA enforcement. */
    bool block_private_network;
    /* Transport-level policy, independent of whether cookie is empty.
       OMIT keeps libcurl's cookie engine disabled, including across redirect
       hops. SAME_ORIGIN evaluates every hop against credential_origin. */
    FetchCredentialPolicy credentials;
    const char *credential_origin;
    /* Immutable navigation/request source used to recompute redirect-hop
       Referrer and Sec-Fetch-Site. referrer_policy uses the standard token
       syntax accepted by fetch_compute_referrer. */
    const char *initiator_url;
    const char *referrer_source;
    const char *referrer_policy;
    /* CORS-mode callers require checks on every cross-origin or
       redirect-origin-tainted response, including the final response before
       any body is exposed. redirect_same_origin_only additionally prevents a
       redirect from forwarding a request body or application headers to a
       different origin. */
    bool enforce_cors;
    /* Long-lived streams may enforce CORS in their headers callback before
       exposing the first byte. Exactly one CORS response gate is required. */
    bool cors_response_check_deferred;
    bool redirect_same_origin_only;
    FetchRedirectPolicy redirect_policy;
    /*
     * Native subsystem trust boundary applied to the initial request and
     * every resolved redirect target before a request is sent. Page fetches
     * do not install this callback.
     */
    FetchRedirectUrlValidator redirect_url_validator;
    /* A conditional module request may inherit omitted CORS response fields
       from a previously validated cache entry on a direct 304. The caller may
       set this only after matching that entry to this exact request URL,
       serialized Origin, and credentials policy; a validator is required.
       Redirected and non-304 responses never consume this provenance. */
    bool cors_cached_response_validated;
    /* Optional browser cookie-policy source. Fetch snapshots only its cookie
       state, evaluates every redirect hop against cookie_context, and stages
       intermediate Set-Cookie fields in that private snapshot. The caller's
       cookie state is not mutated until the completed FetchResult is
       accepted. Verified HTTPS response hops may update the session's bounded
       HSTS policy immediately. The session must therefore outlive an async
       scheduler request. Both pointers must be supplied together; cookie
       remains the bounded fail-closed fallback for callers without a
       BrowserSession. */
    const struct BrowserSession *cookie_session;
    const TilefinchRequestContext *cookie_context;
    /*
     * Non-NULL marks a page-originated request and is the single security
     * context used for destination, mode, credentials, mixed-content, and
     * redirect policy.  Native browser services (updates, the lightweight
     * media provider, trace acquisition) deliberately leave this NULL.
     *
     * The scheduler snapshots this possibly stack-backed value before the
     * enqueue call returns.  cookie_context may alias it, but remains a
     * separate optional pointer because cookie persistence can be disabled.
     */
    const TilefinchRequestContext *page_context;
    const struct TilefinchContentSecurityPolicy *content_security_policy;
    /* Optional engine-lifetime network blocker. Ordinary browser callers may
       omit it when cookie_session carries the same policy. */
    struct ContentBlocker *content_blocker;
    /* Nonzero only for requests emitted by fetch_prepare_page_request(). */
    uint32_t prepared_page_version;
} FetchRequest;

typedef enum {
    FETCH_REQUEST_VALIDATION_OK = 0,
    FETCH_REQUEST_VALIDATION_METHOD,
    FETCH_REQUEST_VALIDATION_HEADER_VALUE,
    FETCH_REQUEST_VALIDATION_EXTRA_HEADERS,
    FETCH_REQUEST_VALIDATION_BODY,
    FETCH_REQUEST_VALIDATION_CREDENTIALS,
    FETCH_REQUEST_VALIDATION_CONTEXT
} FetchRequestValidationError;

#define FETCH_PREPARED_PAGE_REQUEST_VERSION 1u
#define FETCH_PREPARED_PAGE_REQUEST_BYTES 7168u

typedef struct {
    const char *document_url;
    const char *top_level_url;
    /* Defaults to document_url. Module descendants may use their importing
       module URL without changing the request's origin authority. */
    const char *referrer_source_url;
    const char *referrer_policy;
    const struct BrowserSession *session;
    const struct TilefinchContentSecurityPolicy *content_security_policy;
    struct ContentBlocker *content_blocker;
    bool opaque_origin;
} FetchPageSecurityContext;

typedef struct {
    const char *target_url;
    const char *method;
    TilefinchRequestMode mode;
    TilefinchCredentialsMode credentials;
    TilefinchRequestDestination destination;
    bool top_level_navigation;
    bool user_activated;
    /* Transport-only options. Authority-bearing fields are ignored and must
       be zero; the builder derives them from security_context. */
    FetchRequest transport;
} FetchPageRequestDescriptor;

typedef union {
    max_align_t alignment;
    unsigned char bytes[FETCH_PREPARED_PAGE_REQUEST_BYTES];
} FetchPreparedPageRequest;

bool fetch_prepare_page_request(
    const FetchPageSecurityContext *security_context,
    const FetchPageRequestDescriptor *descriptor,
    FetchPreparedPageRequest *prepared,
    FetchRequestValidationError *error);
/* Convenience boundary for callers that already own the complete typed
   request context. transport still contains transport-only options. */
bool fetch_prepare_page_request_context(
    const TilefinchRequestContext *context,
    const char *referrer_source_url, const char *referrer_policy,
    const struct BrowserSession *session,
    const struct TilefinchContentSecurityPolicy *content_security_policy,
    struct ContentBlocker *content_blocker,
    const FetchRequest *transport,
    FetchPreparedPageRequest *prepared,
    FetchRequestValidationError *error);
const FetchRequest *fetch_prepared_page_request(
    const FetchPreparedPageRequest *prepared);
const TilefinchRequestContext *fetch_prepared_page_request_context(
    const FetchPreparedPageRequest *prepared);

/* Applies the transport/replay request-injection boundary without allocating.
   A NULL request is the canonical default GET request. */
bool fetch_request_validate(
    const FetchRequest *request, FetchRequestValidationError *error);

/* Intersects a response's Critical-CH list with supported high-entropy hints
   explicitly opted into by that same response's Accept-CH. Malformed or
   unaccepted response tokens are ignored; an empty output means no retry. */
bool fetch_accepted_critical_client_hints(
    const char *accept_ch, const char *critical_ch,
    char *output, size_t output_size);

typedef bool (*FetchCancelCallback)(void *opaque);
typedef bool (*FetchStreamHeadersCallback)(void *opaque,
                                           const FetchResult *metadata);
typedef bool (*FetchStreamBodyCallback)(void *opaque,
                                        const unsigned char *data,
                                        size_t length);
typedef bool (*FetchStreamStallCallback)(void *opaque);

typedef struct {
    FetchStreamHeadersCallback on_headers;
    FetchStreamBodyCallback on_body;
    FetchStreamStallCallback on_stall;
    void *opaque;
    /* Replay-only delivery controls. Zero selects the canonical 16 KiB
       schedule. A nonzero irregular seed takes precedence over chunk_bytes. */
    size_t chunk_bytes;
    uint64_t irregular_seed;
    size_t irregular_max_chunk_bytes;
    size_t stall_every_chunks;
    /* Deterministic positive fault boundaries. Zero disables each boundary. */
    size_t cancel_after_bytes;
    size_t truncate_after_bytes;
} FetchStreamOptions;

typedef struct {
    size_t bytes_received;
    size_t chunks_received;
    /* Live transports publish libcurl's wire progress when the server makes
       a total available. These remain zero for unknown/chunked totals. */
    size_t download_total_bytes;
    size_t downloaded_bytes;
    size_t peak_buffered_bytes;
    size_t parser_pauses;
    size_t replay_stalls;
    bool headers_delivered;
    bool cancelled;
    bool truncated;
    size_t pump_calls;
    size_t quota_yields;
    size_t maximum_pump_bytes;
    uint64_t maximum_pump_us;
} FetchStreamMetrics;

/* A scheduler pump is intentionally bounded in page-visible body work.  The
   transport may hand libcurl one final fragment after a quota is reached;
   that fragment is retained in a bounded per-request staging buffer and is
   not exposed to the consumer until a later pump.  A zero field selects the
   conservative default (4 callbacks, 64 KiB, and 10 ms respectively). */
typedef struct {
    size_t maximum_body_callbacks;
    size_t maximum_body_bytes;
    uint64_t maximum_time_us;
} FetchPumpQuota;

typedef struct {
    size_t completions;
    size_t body_callbacks;
    size_t body_bytes;
    size_t peak_buffered_bytes;
    uint64_t elapsed_us;
    bool quota_yielded;
} FetchPumpMetrics;

/* Read-only progress for an enqueued request. This deliberately exposes no
   transport handle and is safe to sample between bounded scheduler pumps. */
typedef struct {
    size_t received_body_bytes;
    long status_code;
    bool active;
    bool complete;
} FetchRequestProgress;

/*
 * PSP-only bounded transport lane for work whose DNS/TCP/TLS progress must
 * never run on the browser thread.  The implementation owns one process
 * worker, six request descriptors and bounded response buffers; callers retain
 * all policy, DOM, Budget and filesystem work on the browser thread.
 *
 * Host/replay builds expose the same API as an unavailable capability so the
 * ordinary cooperative FetchScheduler remains the deterministic test path.
 */
#define FETCH_BACKGROUND_REQUEST_LIMIT 6u
#define FETCH_BACKGROUND_ACTIVE_LIMIT 2u
#define FETCH_BACKGROUND_STREAM_ACTIVE_LIMIT 6u
#define FETCH_BACKGROUND_MAXIMUM_RESPONSE_BYTES (256u * 1024u)
#define FETCH_BACKGROUND_STREAM_BUFFER_BYTES (64u * 1024u)

typedef struct {
    size_t received_body_bytes;
    size_t available_body_bytes;
    long status_code;
    bool active;
    bool complete;
} FetchBackgroundProgress;

typedef struct {
    size_t streaming_started;
    size_t fixed_started;
    size_t peak_streaming_active;
    size_t peak_fixed_active;
    size_t worker_performs;
    size_t worker_polls;
    size_t header_callbacks;
    size_t body_callbacks;
    size_t completions;
    unsigned worker_perform_max_us;
    unsigned worker_perform_over_33ms;
    unsigned worker_perform_over_100ms;
    unsigned worker_setup_performs;
    unsigned worker_setup_perform_max_us;
    unsigned worker_steady_perform_max_us;
    unsigned worker_priority_failures;
    unsigned worker_service_max_us;
    unsigned worker_poll_max_us;
    unsigned worker_loop_max_us;
    uint64_t worker_run_clocks;
    unsigned worker_thread_preemptions;
    int worker_priority;
    int last_multi_code;
    int last_running;
} FetchBackgroundTransportMetrics;

typedef struct {
    size_t length;
    size_t received_body_bytes;
    long status_code;
    long negotiated_http_version;
    long new_connections;
    bool success;
    bool timed_out;
    bool tls_handshake_measured;
    uint64_t tls_handshake_us;
    bool tls_connection_reuse_known;
    bool tls_connection_reused;
    bool tls12_compatibility_retry;
    char effective_url[4096];
    char content_range[128];
    char content_type[128];
    char etag[192];
    char last_modified[128];
    char cf_mitigated[32];
    char accept_ch[1024];
    char critical_ch[1024];
    char server[64];
    char cf_ray[64];
    char response_headers[FETCH_RESPONSE_HEADERS_LIMIT];
    size_t response_headers_length;
    FetchResponseSecurityMetadata security;
    char response_referrer_policy[FETCH_REFERRER_POLICY_LIMIT];
    bool response_referrer_policy_header_present;
    bool response_referrer_policy_metadata_valid;
    bool response_security_headers_truncated;
    char error[256];
} FetchBackgroundResult;

/* The native media range reader needs only transport admission and timing
   fields.  Keeping that view separate prevents every range-header poll and
   completed window from zeroing a full FetchResult (including page security,
   client-hint and cookie storage which credential-less media never consumes). */
typedef struct {
    size_t length;
    size_t received_body_bytes;
    long status_code;
    long new_connections;
    bool success;
    bool timed_out;
    bool tls_handshake_measured;
    uint64_t tls_handshake_us;
    bool tls_connection_reuse_known;
    bool tls_connection_reused;
    bool tls12_compatibility_retry;
    char effective_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    char content_length[64];
    char content_range[128];
    char error[256];
} FetchBackgroundMediaResponse;

_Static_assert(sizeof(FetchResponseSecurityMetadata) <= 512u,
               "typed response security metadata exceeded its PSP budget");
_Static_assert(sizeof(FetchResult) < 16u * 1024u,
               "fetch result exceeded its bounded PSP allocation");
_Static_assert(sizeof(FetchBackgroundResult) < 16u * 1024u,
               "background fetch result exceeded its bounded PSP slot");
_Static_assert(sizeof(FetchBackgroundMediaResponse) < 3u * 1024u,
               "media response summary exceeded its PSP stack budget");

bool fetch_background_transport_available(void);
bool fetch_background_transport_initialize(Budget *budget);
/* Pure admission predicate shared by the PSP worker and host media replay.
   It deliberately does not require an initialized worker: host tests use it
   to ensure a request accepted by their scheduler would also be representable
   by the shipping device transport. */
bool fetch_background_transport_stream_shape_supported(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, bool single_hop);
uint64_t fetch_background_transport_enqueue(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms);
uint64_t fetch_background_transport_enqueue_stream(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms);
/* Media can request a smaller publication quantum without imposing its
   latency/throughput tradeoff on navigation and update streams. */
uint64_t fetch_background_transport_enqueue_stream_sized(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, size_t publication_bytes);
/*
 * Browser scheduler form: execute exactly one already-authorized HTTP hop.
 * Redirect responses are returned to the browser thread without following
 * them, so cookie overlays, referrer policy, CORS taint, method rewriting and
 * the next target are decided by the ordinary scheduler policy code.
 */
uint64_t fetch_background_transport_enqueue_hop_stream(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms);
bool fetch_background_transport_hop_supported(
    const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms);
bool fetch_background_transport_progress(
    uint64_t request_id, FetchBackgroundProgress *progress);
bool fetch_background_transport_metrics(
    FetchBackgroundTransportMetrics *metrics);
/* Copies immutable response metadata once the final response headers for the
   current hop are complete. Body ownership and Set-Cookie allocations remain
   with the browser thread and are delivered by the chunk/result APIs. */
bool fetch_background_transport_take_headers(
    uint64_t request_id, FetchResult *metadata);
/* Media-only header view. Consumes the same one-shot header publication as
   take_headers; a caller must select exactly one representation. */
bool fetch_background_transport_take_media_headers(
    uint64_t request_id, FetchBackgroundMediaResponse *metadata);
bool fetch_background_transport_take_chunk(
    uint64_t request_id, unsigned char *destination,
    size_t capacity, size_t *length);
bool fetch_background_transport_take_fetch_result(
    uint64_t request_id, Budget *budget, FetchResult *result);
bool fetch_background_transport_take_fetch_result_consumed(
    uint64_t request_id, Budget *budget, FetchResult *result,
    size_t consumed_body_bytes);
/* Retires a completed streaming media request without materializing page-only
   response metadata or allocating response-cookie copies. The return value
   has the same success semantics as take_fetch_result_consumed. */
bool fetch_background_transport_take_media_result_consumed(
    uint64_t request_id, FetchBackgroundMediaResponse *result,
    size_t consumed_body_bytes);
/* Fixed-body counterpart used by non-streaming media windows. */
bool fetch_background_transport_take_media(
    uint64_t request_id, unsigned char *destination,
    size_t capacity, FetchBackgroundMediaResponse *result);
bool fetch_background_transport_take(
    uint64_t request_id, unsigned char *destination,
    size_t capacity, FetchBackgroundResult *result);
/*
 * Native-service streaming form. The request is deep-copied before enqueue;
 * DNS/TCP/TLS and response production stay on the PSP worker, while body
 * ownership, Budget growth, cookies and cancellation callbacks remain on the
 * calling browser thread. Host/replay callers continue to use the ordinary
 * deterministic transport.
 */
bool fetch_background_request_cancelable(
    Budget *budget, const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, FetchCancelCallback cancel,
    void *cancel_opaque, FetchResult *result);
bool fetch_background_transport_cancel(
    uint64_t request_id, const char *reason);
/* Browser-thread snapshot of operations that still own worker/curl state.
   Teardown treats each as a network-stack lease; the bounded scan includes
   queued, running, completed-not-retired, and speculative preconnect work. */
size_t fetch_background_transport_active_operations(void);
/* Sleeps only the calling PSP thread; transport continues on its worker. */
void fetch_background_transport_wait(unsigned milliseconds);
/* Begin cancellation without waiting, then sample whether every worker/curl
   lease has retired.  Supervisors use this pair to keep teardown pumped. */
void fetch_background_transport_request_quiesce(void);
bool fetch_background_transport_is_quiesced(void);
/* One-shot ENETDOWN-class hint. Callers must confirm with an APCTL/interface
   probe before changing stack state; an origin/CDN failure is not proof. */
bool fetch_background_transport_take_network_regression_hint(void);
/* Supervisor-owned admission gate. Existing operations may drain after it
   closes; new transport leases are refused. */
void fetch_background_transport_set_admission(bool open);
/* Compatibility wrapper: request cancellation and wait for bounded time. */
bool fetch_background_transport_quiesce(unsigned timeout_ms);
/* Controlled-exit teardown. Must run before the owning Budget is destroyed. */
bool fetch_background_transport_shutdown(unsigned timeout_ms);

typedef struct FetchScheduler FetchScheduler;
typedef struct FetchSchedulerDomain FetchSchedulerDomain;

/*
 * A domain bounds occupied request slots across otherwise independent
 * scheduler views.  A slot remains active while a terminal result is waiting
 * for take/discard, because that result still owns the slot's bounded
 * metadata.  Domains and their telemetry are main-thread objects; individual
 * scheduler views retain their own CURLM and pump order.
 */
typedef struct {
    size_t maximum_active_slots;
    size_t active_slots;
    size_t peak_active_slots;
    size_t rejected_enqueues;
    size_t active_views;
    size_t peak_active_views;
} FetchSchedulerDomainMetrics;

/*
 * Handshake attribution accumulated over completed transport hops.
 * `transfers` counts every successful hop that was classified; a hop is
 * classified only when it reached the HTTP layer over TLS, so a connection
 * that never handshook is not silently filed as "reused".  `measured` is the
 * subset for which the backend supplied usable handshake timing, and the two
 * µs totals are drawn from exactly that subset.  A backend that reports no
 * timing therefore leaves the totals at zero with `measured` zero, which
 * reads as "not measured" rather than "measured as instantaneous".
 */
typedef struct {
    size_t transfers;
    size_t full_handshakes;
    size_t reused_connections;
    size_t measured;
    uint64_t total_handshake_us;
    uint64_t maximum_handshake_us;
} FetchTlsHandshakeCounters;

/* Process-wide totals across synchronous fetches and every scheduler. */
void fetch_tls_handshake_counters(FetchTlsHandshakeCounters *counters);
void fetch_tls_handshake_counters_reset(void);
/*
 * Renders the counters as one lowercase key=value fragment in the transport
 * log convention, without a trailing newline.  Unmeasured totals render as
 * "absent" rather than "0" so a log line never implies a measurement that
 * was not taken.  Returns false if the buffer is too small.
 */
bool fetch_tls_handshake_counters_format(
    const FetchTlsHandshakeCounters *counters, char *output,
    size_t output_size);

/*
 * Speculative preconnect (docs/engineering/PSP_TRANSPORT.md).
 *
 * A HOME tile that holds focus during the user's think-time is a host they are
 * about to open.  fetch_preconnect begins a single speculative TCP+TLS
 * connection to that host, parked in the shared transport's connection/session
 * cache, so the handshake is already paid by the time X is pressed.  It is a
 * connect-only handshake: CURLOPT_CONNECT_ONLY, no HTTP request, no bytes sent
 * beyond the TLS handshake, no content fetched.  The warm DNS entry and the
 * TLS session it leaves in the shared cache let the following navigation
 * resume; the bounded TLS session store can also preserve it across boots.
 * On PSP the shared transport worker owns this handshake through a dedicated
 * compact descriptor; it does not consume or reduce the six response lanes.
 *
 * Privacy: the caller resolves the target only from the user's own tiles --
 * their bookmarks or the two built-in cards -- never a page-supplied host.
 *
 * These counters describe preconnect activity itself; the navigation-side
 * payoff (a reused/resumed connection on the real fetch) is reported by the
 * FetchTlsHandshakeCounters above.
 *   started   - a new speculative connection was initiated.
 *   completed - the TCP+TLS handshake finished; the host is warm.
 *   reused    - a preconnect request was satisfied by the speculative
 *               connection already outstanding/warm for that same host, so no
 *               new socket was opened.
 *   cancelled - an in-flight (not-yet-completed) speculative connection was
 *               torn down before it finished (focus moved, quiesce, teardown).
 */
typedef struct {
    size_t started;
    size_t completed;
    size_t reused;
    size_t cancelled;
} FetchPreconnectCounters;

void fetch_preconnect_counters(FetchPreconnectCounters *counters);
void fetch_preconnect_counters_reset(void);

/*
 * Begins at most one speculative connection to the host of url_or_host (a full
 * URL or a bare host; only the scheme/host/port are used).  budget must be the
 * same page budget the browser's navigation transport uses, because the
 * speculative handle is share-attached so its warm connection is the one the
 * real navigation reuses.  Returns true when a speculative connection is
 * outstanding for that host afterward -- freshly started, or already warm (a
 * "reused" no-op).  Inert (returns false) when the transport runtime is
 * unavailable, when handle creation fails (e.g. the hermetic replay/stub
 * build), or when the input is empty or not http(s).  Only one connection is
 * ever outstanding: a request for a different host replaces the current one.
 */
bool fetch_preconnect(const char *url_or_host, Budget *budget);

/* Tears down any outstanding speculative connection now.  reason is for the
   validation log only.  A no-op when nothing is outstanding. */
void fetch_preconnect_cancel(const char *reason);

/* Observes the outstanding worker-owned speculative connection on PSP, or
   drives the deterministic cooperative fallback on host builds. Its
   DNS/TCP/TLS attempt carries an eight-second total deadline; expiry is a
   transparent miss, never a navigation failure. Safe (and cheap) to call at
   every cooperative checkpoint; a no-op once the connection has completed or
   when none is outstanding. */
void fetch_preconnect_pump(void);

/* True while a speculative connection is outstanding or parked warm. */
bool fetch_preconnect_active(void);

/*
 * Pure dwell/eligibility state machine for the HOME preconnect call site,
 * factored out of the PSP-only input path so its bounds are host-testable.
 * It owns no transport; it only decides, per frame, what the caller must do.
 *
 * `eligible` folds every start precondition the caller can see: the HOME
 * surface is active, a preconnectable tile holds focus, the network is READY,
 * the app is not quiescing/suspending, and there is no budget pressure.
 * `tile_key` identifies the focused target (a stable hash of its host) so a
 * focus move between two tiles of the same host does not restart the dwell.
 * `elapsed_ms` is the frame delta; `dwell_threshold_ms` is the settle time
 * (~300 ms) a tile must hold focus before its connection is worth opening.
 */
typedef struct {
    bool armed;          /* a tile is currently accumulating dwell time */
    bool started;        /* START was already emitted this dwell episode */
    uint32_t tile_key;   /* identity of the dwelled target */
    uint32_t elapsed_ms; /* accumulated dwell time on the current target */
} FetchPreconnectDwell;

typedef enum {
    /* Nothing to do this frame. */
    FETCH_PRECONNECT_DWELL_IDLE = 0,
    /* Tear down the outstanding speculative connection (focus left a settled
       tile, moved to another, or the caller became ineligible). */
    FETCH_PRECONNECT_DWELL_CANCEL,
    /* The dwell is satisfied: start the speculative connection now. */
    FETCH_PRECONNECT_DWELL_START
} FetchPreconnectDwellAction;

FetchPreconnectDwellAction fetch_preconnect_dwell_step(
    FetchPreconnectDwell *dwell, bool eligible, uint32_t tile_key,
    uint32_t elapsed_ms, uint32_t dwell_threshold_ms);

/* Convenience hash for deriving a dwell tile_key from a resolved URL/host. A
   zero result is remapped so it never collides with the "no target" sentinel
   callers may use. */
uint32_t fetch_preconnect_tile_key(const char *url_or_host);

typedef enum {
    FETCH_INJECT_NONE = 0,
    FETCH_INJECT_TIMEOUT,
    FETCH_INJECT_TLS,
    FETCH_INJECT_REDIRECT,
    FETCH_INJECT_TRUNCATED,
    FETCH_INJECT_CANCELLED
} FetchInjectedFailure;

/* Deterministic, one-shot lab fault injection. It never fabricates success. */
void fetch_inject_failure_once(FetchInjectedFailure failure);

bool fetch_url(Budget *budget, const char *url, size_t maximum_bytes,
               long timeout_ms, FetchResult *result);
bool fetch_url_cancelable(Budget *budget, const char *url,
                          size_t maximum_bytes, long timeout_ms,
                          FetchCancelCallback cancel, void *cancel_opaque,
                          FetchResult *result);
bool fetch_request_cancelable(Budget *budget, const char *url,
                              const FetchRequest *request,
                              size_t maximum_bytes, long timeout_ms,
                              FetchCancelCallback cancel,
                              void *cancel_opaque, FetchResult *result);
/* Performs exactly one HTTP transaction. A redirect response is returned to
   the caller with its Location metadata and is never followed. This narrow
   host/lab primitive is used by the credentialless trace acquisition tool;
   ordinary browser fetches must continue to use fetch_request_cancelable(). */
bool fetch_request_single_hop(
    Budget *budget, const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, FetchResult *result);
bool fetch_request_stream_cancelable(
    Budget *budget, const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, FetchCancelCallback cancel,
    void *cancel_opaque, const FetchStreamOptions *stream,
    FetchStreamMetrics *metrics, FetchResult *result);
/*
 * Selects an explicit PEM trust bundle for subsequent HTTPS transfers.
 * Passing NULL or an empty string restores libcurl's platform default.
 * The value is process-global like libcurl initialization and cannot change
 * while a scheduler or synchronous transfer owns the transport.
 */
bool fetch_set_ca_bundle_path(const char *path);
const char *fetch_ca_bundle_path(void);
/*
 * Selects the on-stick file backing cross-boot TLS session resumption
 * (docs/engineering/PSP_TRANSPORT.md). Transport teardown
 * updates a bounded in-memory generation; controlled exit/suspend flushes it
 * here, so scheduler churn never performs Memory Stick I/O. The blobs are
 * resumption secrets: stick-only, cleared by CLEAR HTTP CACHES.
 * Passing NULL or an empty string disables the store. Like the CA bundle, the
 * value is process-global and cannot change while the transport is owned.
 */
bool fetch_set_tls_session_store_path(const char *path);
const char *fetch_tls_session_store_path(void);
/* Controls only import/export to the cross-boot store. Disabling removes the
   persisted generation but deliberately leaves live connections and curl's
   process-local TLS cache alone. */
bool fetch_set_tls_session_persistence_enabled(bool enabled);
bool fetch_tls_session_persistence_enabled(void);
bool fetch_tls_session_store_flush(void);
bool fetch_tls_session_store_clear(void);
const char *fetch_transport_version(void);
const char *fetch_transport_tls_version(void);
const char *fetch_transport_http2_version(void);
bool fetch_transport_http2_available(void);
const char *fetch_http_version_name(long negotiated_http_version);
FetchScheduler *fetch_scheduler_create(Budget *budget,
                                       size_t maximum_concurrent,
                                       size_t maximum_reserved_bytes);
/* Diagnostic form used at user-visible navigation boundaries. The ordinary
   constructor remains the compact API for callers whose enclosing subsystem
   already has a more specific error contract. */
FetchScheduler *fetch_scheduler_create_ex(
    Budget *budget, size_t maximum_concurrent,
    size_t maximum_reserved_bytes, char *error, size_t error_size);
FetchSchedulerDomain *fetch_scheduler_domain_create(
    Budget *budget, size_t maximum_active_slots);
FetchSchedulerDomain *fetch_scheduler_domain_retain(
    FetchSchedulerDomain *domain);
void fetch_scheduler_domain_release(FetchSchedulerDomain *domain);
bool fetch_scheduler_domain_metrics(
    const FetchSchedulerDomain *domain,
    FetchSchedulerDomainMetrics *metrics);
FetchScheduler *fetch_scheduler_create_in_domain(
    FetchSchedulerDomain *domain, size_t maximum_concurrent,
    size_t maximum_reserved_bytes);
/* PSP native-service transport is opt-in per scheduler. Replay/host builds
   remain deterministic and report false. This lets top-level navigation move
   first without silently reducing the wider page-resource concurrency. */
bool fetch_scheduler_enable_background_transport(
    FetchScheduler *scheduler, bool enabled);
uint64_t fetch_scheduler_enqueue(FetchScheduler *scheduler, const char *url,
                                 const FetchRequest *request,
                                 size_t maximum_bytes, long timeout_ms);
uint64_t fetch_scheduler_enqueue_stream(
    FetchScheduler *scheduler, const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms,
    const FetchStreamOptions *stream);
/*
 * Reports transient scheduler backpressure for a request that could fit
 * after currently owned slots or reservations are released. Invalid or
 * intrinsically oversized requests return false so callers do not retry
 * them forever.
 */
bool fetch_scheduler_enqueue_would_block(
    const FetchScheduler *scheduler, size_t maximum_bytes);
const char *fetch_scheduler_last_error(const FetchScheduler *scheduler);
size_t fetch_scheduler_pump(FetchScheduler *scheduler,
                            size_t maximum_completions,
                            unsigned maximum_wait_ms);
size_t fetch_scheduler_pump_bounded(
    FetchScheduler *scheduler, size_t maximum_completions,
    unsigned maximum_wait_ms, const FetchPumpQuota *quota,
    FetchPumpMetrics *metrics);
bool fetch_scheduler_take(FetchScheduler *scheduler, uint64_t request_id,
                          bool *success, FetchResult *result);
bool fetch_scheduler_take_stream(
    FetchScheduler *scheduler, uint64_t request_id, bool *success,
    FetchStreamMetrics *metrics, FetchResult *result);
bool fetch_scheduler_stream_metrics(
    const FetchScheduler *scheduler, uint64_t request_id,
    FetchStreamMetrics *metrics);
bool fetch_scheduler_request_progress(
    const FetchScheduler *scheduler, uint64_t request_id,
    FetchRequestProgress *progress);
bool fetch_scheduler_request(FetchScheduler *scheduler, const char *url,
                             const FetchRequest *request,
                             size_t maximum_bytes, long timeout_ms,
                             FetchResult *result);
bool fetch_scheduler_cancel(FetchScheduler *scheduler, uint64_t request_id,
                            const char *reason);
/* Completes every active request as cancelled while retaining each result
   for its ordinary owner to consume and reject. Returns the requests closed. */
size_t fetch_scheduler_cancel_all(
    FetchScheduler *scheduler, const char *reason);
/* Releases a completed/cancelled result in place without copying its bounded
   response object into caller storage. Useful on teardown and OOM paths. */
bool fetch_scheduler_discard(FetchScheduler *scheduler,
                             uint64_t request_id);
size_t fetch_scheduler_pending(const FetchScheduler *scheduler);
/* Handshake attribution for the transport hops this scheduler completed. */
bool fetch_scheduler_tls_handshake_counters(
    const FetchScheduler *scheduler, FetchTlsHandshakeCounters *counters);
void fetch_set_blocked_origins(const char *const *hosts, size_t count);
bool fetch_origin_blocked(const char *url);
void fetch_scheduler_debug_dump(const FetchScheduler *scheduler,
                                const char *label);
void fetch_scheduler_reservation_state(const FetchScheduler *scheduler,
                                       size_t *reserved_bytes,
                                       size_t *maximum_reserved_bytes,
                                       size_t *domain_active_slots,
                                       size_t *domain_maximum_slots);
/* Reports whether `maximum_bytes` would be accepted by the byte pool right
   now AND still leave `reserve_bytes` of it free afterwards. Every request
   reserves its whole declared limit from enqueue until completion, so a
   speculative lane that only asks "does it fit" can consume the room an
   authoritative fetch is about to need. Speculation passes the headroom it
   must preserve; `reserve_bytes == 0` is the plain enqueue question. */
bool fetch_scheduler_reservation_available(const FetchScheduler *scheduler,
                                           size_t maximum_bytes,
                                           size_t reserve_bytes);
bool fetch_scheduler_uses_virtual_replay(const FetchScheduler *scheduler);
void fetch_scheduler_destroy(FetchScheduler *scheduler);
bool fetch_trace_capture_begin(const char *directory, char *error,
                               size_t error_size);
/* Arms capture to begin immediately before the Nth top-level document
   request. Preparatory traffic remains live and unrecorded; activation writes
   a redacted cookie-jar seed before record 0000. */
bool fetch_trace_capture_arm_top_level(
    const char *directory, size_t request_ordinal,
    char *error, size_t error_size);
bool fetch_trace_replay_begin(const char *directory, char *error,
                              size_t error_size);
/* Lab-only response replay for visual comparisons. Unlike the strict API
   above, this mode selects a retained response by method plus normalized
   request URL and deliberately ignores the captured request-header shape.
   Successful nonzero HTTP records outrank retained HTTP and status-zero
   failures. The highest-rank records are claimed in record order and fail
   closed after exhaustion; a singleton is reusable. Integrity-valid retained
   failures are matched rejections and never expose their body or cookies.
   Production behavior and ordinary replay tests continue to use the strict
   API. */
bool fetch_trace_replay_begin_response_keyed(
    const char *directory, char *error, size_t error_size);
#define FETCH_TRACE_RESPONSE_KEY_ROUTE_SELECTION_VERSION \
    "ranked-occurrence-v2"
typedef struct {
    bool response_keyed;
    size_t record_count;
    size_t claimed_record_count;
    size_t request_count;
    size_t matched_request_count;
    size_t served_request_count;
    /* Matched records which completed as integrity-valid retained failures or
       whose successful response was withheld by browser policy. */
    size_t rejected_request_count;
    size_t unmatched_request_count;
    size_t conflicting_request_count;
    size_t invalid_route_request_count;
    size_t request_shape_mismatch_count;
    /* A highest-rank singleton is reusable for every matching request.
       Multiple highest-rank records are claimed once each in record order;
       exhaustion is a fail-closed route conflict. */
    size_t occurrence_claim_count;
    size_t reusable_claim_count;
    size_t occurrence_exhausted_count;
} FetchTraceReplayStats;
/* Read-only lab diagnostics for an active replay. claimed_record_count counts
   unique record IDs, while reusable_claim_count can resolve one claimed ID
   repeatedly. A rejected request either reproduced an integrity-valid
   retained failure or matched a successful retained record whose response
   browser policy withheld. Every response-keyed request has exactly one of
   the matched, unmatched, conflicting, or invalid terminal outcomes. */
bool fetch_trace_replay_stats(FetchTraceReplayStats *stats);

/* True while a hermetic HTTP replay is active.  Consumers may treat
   stale cache entries as authoritative: revalidating against a trace
   whose records are already claimed can only fail, and replay time is
   logical anyway. */
bool fetch_trace_replay_active(void);
bool fetch_trace_replay_record_was_claimed(size_t sequence);
/* Applies an optional redacted initial cookie seed from an active replay to
   the empty session. Legacy traces without a seed are a successful no-op. */
bool fetch_trace_replay_seed_session(
    struct BrowserSession *session, char *error, size_t error_size);
void fetch_trace_end(void);
bool fetch_trace_clock_origin_ms(uint64_t *origin_ms);
FetchResult *fetch_result_create(Budget *budget);
/* Converts an owned response payload to the main-thread immutable shared-body
   representation in place.  Existing shared results are accepted only when
   their pointer/length invariant is intact.  The payload remains owned by the
   FetchResult until its shared_body reference is retained or transferred. */
bool fetch_result_share_body(FetchResult *result);
void fetch_result_destroy(FetchResult *result);
void fetch_result_free(FetchResult *result);
bool fetch_resolve_url(const char *base_url, const char *reference,
                       char *output, size_t output_size);
bool fetch_response_header_value(const FetchResult *result,
                                 const char *name,
                                 char *output, size_t output_size);
/* Authorizes one completed final response for its typed page-resource sink.
   cors_validated may be true only when the transport enforced CORS for this
   exact final response. mime_matches_destination is supplied by the owning
   decoder/compiler; modules should also set require_strict_mime because their
   MIME check is unconditional. Classic scripts and styles become strict when
   X-Content-Type-Options is `nosniff`. No allocation or page-visible work is
   performed. */
bool fetch_resource_grant_create(
    const FetchResult *result, const TilefinchRequestContext *context,
    bool cors_validated, bool mime_matches_destination,
    bool require_strict_mime, TilefinchResourceGrant *grant,
    TilefinchResourceDeniedReason *denied_reason);
/* Rebuilds authority for a 304 by applying any security fields present on
   the 304 over the cached representation's normalized grant. The previous
   grant is fallback metadata, never the returned authority itself. */
bool fetch_resource_grant_revalidate_304(
    const FetchResult *result, const TilefinchRequestContext *context,
    const TilefinchResourceGrant *previous_grant, bool cors_validated,
    bool mime_matches_destination, bool require_strict_mime,
    TilefinchResourceGrant *grant,
    TilefinchResourceDeniedReason *denied_reason);
/* Reads the dedicated Referrer-Policy metadata accumulated from every field
   in wire order. Legacy replay/synthetic results without that metadata fall
   back to the bounded response-header snapshot. On success, header_present
   reports whether the field occurred at all and policy holds the last
   recognized token, normalized to lowercase. A present field with only empty
   or unknown tokens succeeds with an empty policy. */
bool fetch_response_referrer_policy(
    const FetchResult *result, bool *header_present,
    char policy[FETCH_REFERRER_POLICY_LIMIT]);

/* Implements the CORS protocol's response-sharing check.  request_origin is
   the exact serialized Origin value sent on the corresponding request.  A
   wildcard is accepted only when credentials were not included.  Duplicate
   or malformed ACAO/ACAC fields fail closed. */
bool fetch_cors_response_allows(const FetchResult *result,
                                const char *request_origin,
                                bool credentials_included);
static inline const char *fetch_set_cookie_url(const FetchResult *result,
                                                size_t index,
                                                const char *fallback_url)
{
    if (result != NULL && index < result->set_cookie_count
        && index < FETCH_RESPONSE_COOKIE_CAPACITY
        && result->set_cookie_urls[index] != NULL
        && result->set_cookie_urls[index][0] != '\0') {
        return result->set_cookie_urls[index];
    }
    return fallback_url;
}
bool fetch_compute_referrer(const char *document_url,
                            const char *target_url,
                            const char *policy,
                            char *output, size_t output_size);

#endif
