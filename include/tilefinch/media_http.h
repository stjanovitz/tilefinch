#ifndef TILEFINCH_MEDIA_HTTP_H
#define TILEFINCH_MEDIA_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/media_mp4.h"
#include "tilefinch/session.h"

typedef bool (*MediaHttpRangeTransport)(
    void *opaque, uint64_t first_byte, uint64_t last_byte,
    unsigned char *destination, size_t capacity, size_t *length,
    uint64_t *complete_length, char *error, size_t error_size);

typedef bool (*MediaHttpUrlValidator)(const char *url);

typedef struct {
    size_t cache_bytes;
    /* Optional owned successor windows. Allocation stops at the first budget
       refusal and the admitted prefix remains fully functional. Windows above
       the initial depth are admitted lazily when the runtime limit is raised. */
    unsigned lookahead_windows;
    /* Initial speculative successor depth. Zero means every admitted slot. */
    unsigned initial_lookahead_windows;
    /* Scheduler-side publication quantum for streamed successors. Zero uses
       16 KiB. Native media chooses that latency-oriented value explicitly;
       deterministic replay can raise it to the transport's ordinary 48 KiB
       navigation quantum to measure the device tradeoff. */
    size_t stream_publication_bytes;
    /* A connection below this sustained body rate cannot keep this track
       playing. Zero uses the conservative fixed 8 KiB/s legacy floor. */
    size_t minimum_sustained_bytes_per_second;
    long timeout_ms;
    /*
     * Per-attempt connect bound. Range reads run on the interactive thread,
     * so the connect phase -- the part of a request in which the cancel
     * callback cannot be reached -- has to be much shorter than the request
     * deadline. Zero keeps the transport default of half the deadline.
     */
    long connect_timeout_ms;
    const char *referer;
    /* Ordinary HTML media uses the HTTP Range header. Provider streams keep
       the historical query-range form because that is the URL they sign. */
    bool standard_range_header;
    /* Ask for resumable TLS 1.2 connections (FetchRequest's
       tls12_session_resumption). Set for googlevideo streams, whose
       reconnects otherwise repeat a full handshake. */
    bool tls12_session_resumption;
    /* Select the request representation for an audio-only source. This does
       not change the common range/cache machinery or its bounds. */
    bool audio_only;
    /* When non-NULL, build every range through the page request-authority
       boundary. The prepared envelope is retained by the range object, not
       rebuilt on the playback hot path. */
    const TilefinchRequestContext *page_request_context;
    const char *referrer_policy;
    const struct TilefinchContentSecurityPolicy *content_security_policy;
    /*
     * Optional trust-boundary check applied both to the requested URL and
     * the final effective URL after redirects. A rejected response never
     * enters the bounded media cache.
     */
    MediaHttpUrlValidator url_validator;
    MediaHttpRangeTransport transport;
    void *transport_opaque;
    FetchCancelCallback cancel;
    void *cancel_opaque;
} MediaHttpRangeOptions;

typedef struct {
    size_t requests;
    size_t cache_hits;
    size_t bytes_received;
    size_t retained_bytes;
    size_t failures;
    size_t retry_attempts;
    /* Windows requested before the active one ran out, and reads that were
       answered WOULD_BLOCK because the window had not landed yet. A healthy
       stream shows readahead_requests tracking window_installs and very few
       would_block_reads. */
    size_t readahead_requests;
    /* Aggregate reasons a sequential refill did or did not leave the
       process. These keep device diagnosis out of per-read logging. */
    size_t readahead_checks;
    size_t readahead_waiting_for_consumption;
    size_t readahead_issue_refusals;
    /* Local worker pressure is not a failed HTTP transaction. These count
       bounded issue deferrals and expose whether the current read is waiting
       for a descriptor so buffering policy can remain truthful. */
    size_t admission_deferrals;
    bool admission_deferred;
    size_t window_installs;
    size_t would_block_reads;
    /*
     * Where a "non-blocking" transport step actually went.
     *
     * range_pump is documented as one bounded step and is bounded for
     * transfers, but libcurl's own progress call is only non-blocking for the
     * parts it drives -- a synchronous resolve or handshake inside
     * curl_multi_perform is neither bounded nor visible from above it. A
     * device cycle recorded a single pump unit blocking 478ms with the
     * demuxer's would-block contract demonstrably intact on both of its
     * paths, so the stall is below them and these say which call it was.
     */
    size_t pump_calls;
    uint64_t pump_max_us;
    uint64_t pump_total_us;
    uint64_t install_max_us;
    /*
     * The answer to whether each 256 KiB window opens a fresh connection or
     * reuses one. curl's own CURLINFO_NUM_CONNECTS per completed window: zero
     * means the connection was reused, non-zero means a new TCP+TLS handshake.
     * A device cycle read a pump blocking 9-15ms per call throughout
     * playback, which is the shape of a handshake per window rather than one
     * kept-alive connection across the session -- this says which it is,
     * rather than inferring it from a stall duration.
     */
    size_t window_new_connections;
    size_t window_handshakes;
    uint64_t handshake_max_us;
    /* The last completed window's negotiated CURL_HTTP_VERSION_*: HTTP/2
       lets the video and audio ranges share one connection and handshake.
       tls12_retries counts windows that paid a second full handshake after
       a TLS 1.3 attempt failed. */
    long http_version;
    size_t tls12_retries;
    /* curl connection id of the last completed window, and how many times
       it changed: equal video/audio ids mean the two streams share one
       multiplexed connection (and one TLS handshake). */
    long long connection_id;
    size_t connections_seen;
    /* Bytes of the outstanding window the transport has accepted. Sampled at
       call time, not accumulated: it is the only progress a refill shows
       before its window is installed. */
    size_t bytes_in_flight;
    bool window_pending;
    long last_http_status;
    uint64_t last_first_byte;
    uint64_t last_last_byte;
    /* Windows abandoned and re-issued because a demanded transfer stopped
       making byte progress. Appended. */
    size_t reconnects;
    /* Subset taken only after the decoder was already waiting and the
       transfer made no byte progress for the short starvation bound. */
    size_t starved_reconnects;
    /* A candidate which made no useful progress across every bounded fresh
       connection allowance. This is terminal for the demanded window: it is
       never reported while that same request remains pending. */
    size_t stalled_reconnect_exhaustions;
    bool delivery_stalled;
    size_t minimum_sustained_bytes_per_second;
    /*
     * How long a window spends outstanding, and how much of that the decoder
     * actually waited through. Appended.
     *
     * window_pending says only that a window is on the wire right now, which
     * is the normal steady state of a read-ahead and is not by itself a
     * problem -- so a sampled flag could never distinguish a refill that
     * overlapped playback perfectly from one the pipeline sat on. These do:
     * `pending` is issue -> install for every window, and `starved` is the
     * part of it after the first read that had to answer would-block, which
     * is the interval in which the decoder had nothing to decode.
     */
    size_t window_pending_samples;
    uint64_t window_pending_total_us;
    uint64_t window_pending_max_us;
    uint64_t window_starved_total_us;
    uint64_t window_starved_max_us;
    /*
     * Readahead which was already on the wire when a non-sequential or
     * boundary-spanning read requested a different window. `complete` is the
     * dangerous subset: every byte had arrived and was nevertheless thrown
     * away. `bytes` is transport-accepted data discarded across both cases.
     * A healthy sequential playback run should keep all three at zero.
     */
    size_t readahead_superseded;
    size_t completed_readahead_superseded;
    size_t superseded_bytes;
    /* Logical reads completed atomically from the tail of the installed
       window and the head of its completed sequential successor. */
    size_t split_window_bridges;
    size_t lookahead_installs;
    size_t lookahead_promotions;
    size_t lookahead_retained_bytes;
    /* Reads completed from a header-admitted sequential response before its
       complete range window reached EOF. This is the evidence that a slow
       tail no longer withholds an already-arrived video sample. */
    size_t streaming_partial_reads;
    size_t streaming_partial_bytes;
    /* Live diagnostic coordinates. These are snapshots rather than
       cumulative telemetry, and let a stalled non-blocking demux distinguish
       a slow fill from cache-window ping-pong without logging every read. */
    uint64_t cache_offset;
    size_t cache_length;
    size_t cache_consumed;
    unsigned lookahead_slots;
    unsigned lookahead_fetch_limit;
    bool aggressive_readahead;
    uint64_t fill_offset;
    size_t fill_length;
    uint64_t last_read_offset;
    size_t last_read_length;
} MediaHttpRangeStats;

/* The throughput floor/window remain diagnostic values reported with the
   selected track. They never discard useful bytes: liveness below is based
   only on no byte progress and an absolute demanded-window deadline. */
#define MEDIA_HTTP_TRICKLE_FLOOR_BYTES_PER_SECOND 8192u
#define MEDIA_HTTP_TRICKLE_WINDOW_US 8000000u
/* Playback is already visibly blocked in this case, so the first fresh
   connection should be tried promptly. Later attempts back off: repeatedly
   tearing down a radio path during a longer association or CDN outage can
   prevent the very recovery playback is waiting for. */
#define MEDIA_HTTP_STARVED_NO_PROGRESS_US 2000000u
/* Enough to survive a CDN that paces the first replacement too, and few
   enough that a dead link still reaches the caller's watchdog on schedule. */
#define MEDIA_HTTP_TRICKLE_MAXIMUM_RECONNECTS 3u
/* One byte cannot keep an incident alive forever. This deadline begins when
   playback first demands the window and spans every fresh-connection retry.
   Slow but useful Wi-Fi keeps its bytes and may complete within the bound. */
#define MEDIA_HTTP_DEMANDED_WINDOW_DEADLINE_US 30000000u

static inline uint64_t media_http_window_reconnect_delay_us(
    unsigned reconnects)
{
    /* Three attempts at 2, 4 and 8 seconds without progress. Once they are
       spent, retain the last request until the incident's absolute deadline;
       useful bytes can still recover it, while one-byte trickles cannot keep
       it alive forever. */
    if (reconnects >= MEDIA_HTTP_TRICKLE_MAXIMUM_RECONNECTS)
        return UINT64_MAX;
    return (uint64_t) MEDIA_HTTP_STARVED_NO_PROGRESS_US << reconnects;
}

typedef enum {
    MEDIA_HTTP_WINDOW_WAIT = 0,
    MEDIA_HTTP_WINDOW_RECONNECT,
    MEDIA_HTTP_WINDOW_FAIL
} MediaHttpWindowLivenessAction;

/* Transport-independent state for one logical demanded range window. Tests
   feed this tracker synthetic clocks and byte counts; the HTTP implementation
   feeds it the same observations from curl. It deliberately owns no request
   handle and performs no I/O. */
typedef struct {
    uint64_t demanded_started_us;
    uint64_t last_progress_us;
    size_t last_progress_bytes;
    unsigned reconnects;
    bool demanded;
} MediaHttpWindowTracker;

/* Pure form of the demanded-window liveness policy used by the range source
   and its deterministic fault tests. The incident deadline is deliberately
   independent of byte progress so a one-byte trickle cannot evade the bound. */
static inline MediaHttpWindowLivenessAction media_http_window_liveness(
    bool complete, bool demanded, uint64_t no_progress_us,
    uint64_t demanded_us, unsigned reconnects)
{
    if (complete || !demanded) return MEDIA_HTTP_WINDOW_WAIT;
    if (demanded_us >= MEDIA_HTTP_DEMANDED_WINDOW_DEADLINE_US) {
        return MEDIA_HTTP_WINDOW_FAIL;
    }
    if (no_progress_us
        >= media_http_window_reconnect_delay_us(reconnects))
        return MEDIA_HTTP_WINDOW_RECONNECT;
    return MEDIA_HTTP_WINDOW_WAIT;
}

static inline void media_http_window_tracker_reset(
    MediaHttpWindowTracker *tracker)
{
    if (tracker == NULL) return;
    *tracker = (MediaHttpWindowTracker) {0};
}

static inline void media_http_window_tracker_request_started(
    MediaHttpWindowTracker *tracker, uint64_t now_us)
{
    if (tracker == NULL) return;
    tracker->last_progress_us = now_us;
    tracker->last_progress_bytes = 0;
}

static inline void media_http_window_tracker_demand(
    MediaHttpWindowTracker *tracker, uint64_t now_us)
{
    if (tracker == NULL || tracker->demanded) return;
    tracker->demanded = true;
    tracker->demanded_started_us = now_us;
}

static inline void media_http_window_tracker_reconnected(
    MediaHttpWindowTracker *tracker)
{
    if (tracker != NULL && tracker->reconnects != UINT32_MAX)
        tracker->reconnects++;
}

static inline MediaHttpWindowLivenessAction
media_http_window_tracker_observe(
    MediaHttpWindowTracker *tracker, uint64_t now_us,
    size_t received_bytes, bool complete, bool request_active)
{
    if (tracker == NULL) return MEDIA_HTTP_WINDOW_FAIL;
    if (request_active
        && received_bytes > tracker->last_progress_bytes) {
        tracker->last_progress_bytes = received_bytes;
        tracker->last_progress_us = now_us;
    }
    uint64_t no_progress_us = request_active
        && now_us > tracker->last_progress_us
        ? now_us - tracker->last_progress_us : 0;
    uint64_t demanded_us = tracker->demanded
        && now_us > tracker->demanded_started_us
        ? now_us - tracker->demanded_started_us : 0;
    return media_http_window_liveness(
        complete, tracker->demanded,
        request_active ? no_progress_us : 0,
        demanded_us, tracker->reconnects);
}

typedef struct MediaHttpRange MediaHttpRange;

typedef enum {
    MEDIA_HTTP_RANGE_PRIME_PENDING = 0,
    MEDIA_HTTP_RANGE_PRIME_READY,
    MEDIA_HTTP_RANGE_PRIME_FAILED
} MediaHttpRangePrimeStatus;

/*
 * Build the googlevideo-style range query without duplicating an existing
 * range parameter or appending after a URL fragment. Exposed so offline
 * downloads use exactly the same transport contract as streaming playback.
 */
bool media_http_build_range_url(
    const char *url, uint64_t first_byte, uint64_t last_byte,
    char *output, size_t output_size);
bool media_http_build_range_header(
    uint64_t first_byte, uint64_t last_byte,
    char *output, size_t output_size);
bool media_http_parse_content_range(
    const char *value, uint64_t expected_first, uint64_t expected_last,
    size_t body_length, uint64_t *complete_length);

/*
 * The page-media probe: what an address with no HLS sign answers decides
 * its route. It asks for this many first bytes by Range, enough to see a
 * playlist's #EXTM3U (after a BOM or blank lines) or an MP4's first box
 * type, so a playlist served as text/plain or octet-stream by a server
 * that honours Range is still recognized. A server that ignores Range
 * answers 200; the caller caps that body.
 */
#define MEDIA_HTTP_PROBE_RANGE_BYTES 1024u
bool media_http_build_probe_range_header(char *output, size_t output_size);
/* The probe reads at most this much: a 206 answers its range, and a server
   that ignores Range for a small playlist or an error page still shows
   what it is. */
#define MEDIA_HTTP_PROBE_SNIFF_BYTES (32u * 1024u)
/* Starts the probe on the worker transport: a private single-request
   scheduler whose byte reservation covers the whole sniff window (one
   sized below it refuses every probe). Returns the request id and the
   scheduler (the caller pumps, takes and destroys it), or 0 with
   *scheduler NULL. */
uint64_t media_http_probe_enqueue_background(
    Budget *budget, const char *url, const FetchRequest *request,
    long timeout_ms, FetchScheduler **scheduler);

typedef enum {
    /* No byte-range authority, or nothing usable came back. */
    MEDIA_HTTP_PROBE_REFUSED = 0,
    /* A 206 the range source can read; *complete_length is the size. */
    MEDIA_HTTP_PROBE_RANGE,
    /* A playlist: open the address as HLS. */
    MEDIA_HTTP_PROBE_HLS,
    /* 404/410 or an error page: nothing to play. */
    MEDIA_HTTP_PROBE_UNAVAILABLE,
    /* MPEG audio (media_http_probe_route_audio only). */
    MEDIA_HTTP_PROBE_MP3
} MediaHttpProbeRoute;

/* Routes a completed probe response. A playlist routes to HLS only when
   hls_allowed. Set-Cookie headers of a response routed to HLS or to the
   range source are kept in session under context, as for any page media
   response, so a cookie-gated stream's next request carries them. */
MediaHttpProbeRoute media_http_probe_route(
    BrowserSession *session, const TilefinchRequestContext *context,
    bool hls_allowed, const FetchResult *result, bool fetched,
    uint64_t *complete_length);
/*
 * Page audio's probe. An MPEG audio type or body routes to MP3: a 206 with
 * its whole length and *seekable set, or a 200 whose whole body arrived
 * with that body's length. A body that overflowed the sniff window answered
 * a 1 KiB range with something endless (a radio stream) or with a whole
 * file (a server that ignores Range); unless its status arrived and is not
 * 2xx, it routes to MP3 with length 0, the stream's own response is
 * admitted when it opens, and a declared length ends it
 * (MediaMp3Transport.declared_length). Everything else routes as
 * media_http_probe_route without HLS.
 */
MediaHttpProbeRoute media_http_probe_route_audio(
    BrowserSession *session, const TilefinchRequestContext *context,
    const FetchResult *result, bool fetched, uint64_t *complete_length,
    bool *seekable);

typedef enum {
    /* Nothing to ask: the failure stands as it is. */
    MEDIA_INSECURE_AUDIO_NONE = 0,
    /* Ask once whether this plain-HTTP stream may play. */
    MEDIA_INSECURE_AUDIO_ASK,
    /* The user said no for this site this session. */
    MEDIA_INSECURE_AUDIO_DECLINED
} MediaInsecureAudioStep;

/*
 * After page audio's probe refused or found nothing to play: whether that
 * is a secure page's plain-HTTP address the one HTTPS upgrade could not
 * reach, and what the user's session answer for the page's site makes of
 * it (SECURITY_MODEL.md, "Unencrypted audio"). Potentially trustworthy
 * addresses (loopback) are never upgraded, so never asked about. An
 * ALLOWED answer already sent the probe as HTTP; its failure stands.
 */
MediaInsecureAudioStep media_http_insecure_audio_step(
    const BrowserSession *session, const char *source_url,
    const char *document_url, MediaHttpProbeRoute route);

/*
 * A single-window, bounded HTTP range source. The cache is deliberately
 * constant-size: MP4 metadata and samples are read on demand without ever
 * retaining the complete movie in the page budget.
 */
MediaHttpRange *media_http_range_create(
    Budget *budget, BrowserSession *session, const char *url,
    uint64_t content_length, const MediaHttpRangeOptions *options,
    char *error, size_t error_size);
/* Admit the first metadata window without waiting for it. This is a hint, not
   a new failure boundary: a deferred worker descriptor is retried by the
   ordinary blocking metadata read. */
bool media_http_range_prefetch_metadata(MediaHttpRange *range);
MediaRangeReader media_http_range_reader(MediaHttpRange *range);
/*
 * Whether the whole of [offset, offset + length) sits in the window loaded
 * right now, so a non-blocking read of it would complete.
 *
 * O(1), copies nothing, and cannot start a fetch or a readahead -- it is the
 * observation form of the residency test buried inside range_read_bounded, and
 * exists so instrumentation can ask about bytes it is deliberately not going
 * to read.
 */
bool media_http_range_resident(const MediaHttpRange *range,
                               uint64_t offset, size_t length);
/*
 * Give the outstanding window one bounded, non-blocking step of transport
 * progress, and start the next one if the active window is running low. Callers
 * that pump every frame keep the refill overlapping decode work even in frames
 * where no sample was read. Returns true while a window is outstanding.
 */
bool media_http_range_pump(MediaHttpRange *range);
/*
 * Start proving that the source can serve bytes beyond its first cache
 * window.
 *
 * Some signed media candidates return a valid prefix and reject every later
 * query range.  A normal MP4 open can therefore succeed and fail only after
 * seconds of visible playback. This bounded check reuses the ordinary
 * successor window and retains it as lookahead. A PSP open may continue while
 * it reports PENDING: the same request remains owned by the range source and
 * is completed by ordinary playback pumps rather than becoming a startup
 * latency gate.
 */
MediaHttpRangePrimeStatus media_http_range_prime_successor(
    MediaHttpRange *range);
/* Ask for the sequential successor earlier while playback is intentionally
   buffering. This changes no allocation; metadata/seek reads retain the
   conservative quarter-window rule until callers opt in. */
void media_http_range_set_aggressive_readahead(
    MediaHttpRange *range, bool enabled);
/* Bound speculative successor requests independently of buffers already
   admitted. Reducing the limit retains completed bytes. Increasing it first
   tries any optional lazy allocation, then may start the next sequential
   window; allocation refusal leaves the lower depth operational. */
void media_http_range_set_lookahead_limit(
    MediaHttpRange *range, unsigned windows);
/*
 * Bound every blocking read this source performs from now on to `budget_us`
 * of wall clock, shared across all of them.
 *
 * A blocking read arms its own deadline per window, which is the right bound
 * for one read and no bound at all for a caller that performs many. Opening an
 * MP4 performs an unbounded number of them -- the top-level scan, the moov,
 * every lazy index window -- so a source that answered each read just inside
 * its fifteen-second timeout kept the open alive indefinitely while looking
 * healthy from the inside. The transaction that owns those reads is the only
 * layer that knows what the whole sequence may spend, so it says.
 *
 * A budget of zero is a spent budget, not an absent one: the next blocking
 * read fails immediately rather than waiting out one more window. Use
 * media_http_range_clear_wait_budget to hand the source back to its ordinary
 * per-window `timeout_ms` once the transaction is over.
 *
 * Cancellation is unaffected and remains the fast path: it is observed once
 * per bounded wait step regardless of either deadline.
 */
void media_http_range_set_wait_budget_us(
    MediaHttpRange *range, uint64_t budget_us);
void media_http_range_clear_wait_budget(MediaHttpRange *range);
bool media_http_range_stats(const MediaHttpRange *range,
                            MediaHttpRangeStats *stats);
/*
 * Which lane a range's windows take.
 *
 * A page range (one created with page_request_context) carries the page's
 * authority, which must be applied on every request and every redirect hop:
 * per-hop cookies and Set-Cookie, Referer, the CSP snapshot, the content
 * blocker, mixed content, the private-network check and redirect target
 * admission. Only FetchScheduler's browser-thread hop walk does all of that,
 * so a page range always runs on its own scheduler; when the PSP transport
 * worker exists, that scheduler executes each single hop on the worker
 * (media descriptor priority), so neither DNS/TLS nor the body ever run on
 * the browser thread. A provider range (no page authority, a fixed-origin
 * signed URL and a URL validator) keeps the worker's own fixed/stream media
 * path. Hosts without the worker, and trace replay, use a plain scheduler.
 */
typedef enum {
    MEDIA_HTTP_RANGE_ROUTE_TRANSPORT = 0,
    MEDIA_HTTP_RANGE_ROUTE_SCHEDULER,
    MEDIA_HTTP_RANGE_ROUTE_PAGE_SCHEDULER_WORKER_HOPS,
    MEDIA_HTTP_RANGE_ROUTE_WORKER
} MediaHttpRangeRoute;
MediaHttpRangeRoute media_http_range_route_select(
    bool substituted_transport, bool page_authority, bool worker_available);
#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
/* The route a range took and its name: host tests and the PSP validation
   log only. */
MediaHttpRangeRoute media_http_range_route(const MediaHttpRange *range);
const char *media_http_range_route_name(MediaHttpRangeRoute route);
#endif
/* The window error for a worker enqueue refusal: a request shape the worker
   cannot carry, a worker that is not running, and a full queue read
   differently. (A full queue or a closed admission is normally deferred and
   retried rather than reported.) */
const char *media_http_background_refusal_reason(
    FetchBackgroundEnqueueStatus status);
#if !defined(__PSP__)
/* Host seam: the worker is stubbed on host, so a test forces the
   availability the route decision sees (1 available, 0 not, -1 the real
   answer). Affects ranges created afterwards only. */
void media_http_test_force_background_available(int available);
#endif
/* Conservative duration represented by unread bytes in the installed cache,
   its bounded contiguous successor windows, and a complete sequential result
   waiting in the scheduler. This average-bitrate estimate is for buffering
   UI hysteresis only; demux and presentation never depend on it. */
uint64_t media_http_range_buffered_ahead_us(
    const MediaHttpRange *range, uint64_t duration_us);
void media_http_range_destroy(MediaHttpRange *range);

#endif
