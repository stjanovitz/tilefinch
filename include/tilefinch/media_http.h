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
    /*
     * Optional second owned window. Once a sequential readahead lands it is
     * moved here, allowing the transport to start the following window while
     * playback is still consuming the active one. Zero keeps the historical
     * active+in-flight shape. Allocation failure is non-fatal and degrades to
     * that shape; callers use this only for the lower-bitrate video track.
     */
    unsigned lookahead_windows;
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
    /* Bytes of the outstanding window the transport has accepted. Sampled at
       call time, not accumulated: it is the only progress a refill shows
       before its window is installed. */
    size_t bytes_in_flight;
    bool window_pending;
    long last_http_status;
    uint64_t last_first_byte;
    uint64_t last_last_byte;
    /* Windows abandoned and re-issued because the transfer had slowed to a
       rate no stream can play at. Appended. */
    size_t reconnects;
    /* Subset taken only after the decoder was already waiting and the
       transfer made no byte progress for the short starvation bound. */
    size_t starved_reconnects;
    /* A candidate which made no useful progress across every bounded fresh
       connection allowance. The session may replace its signed URL once;
       ordinary slow-but-progressing Wi-Fi never sets this field. */
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
    bool aggressive_readahead;
    uint64_t fill_offset;
    size_t fill_length;
    uint64_t last_read_offset;
    size_t last_read_length;
} MediaHttpRangeStats;

/*
 * When a window still on the wire has stopped being worth waiting for.
 *
 * A far-offset window on a connection whose CDN burst allowance is spent does
 * not fail -- it trickles. A device soak measured an audio window moving about
 * 16 KiB in six seconds at 168s into a stream, which is a 256 KiB window
 * sixteen seconds away while the media clock needs it in four; playback held
 * video back to keep the interleave and the whole session died on a watchdog
 * that was, for once, correct. The remedy is a new connection, which gets a
 * new burst -- but only for a transfer that has actually stopped delivering.
 *
 * Device evidence also showed that a two-second/24 KiB/s verdict was too
 * aggressive for the PSP's bursty Wi-Fi: it replaced 17 otherwise progressing
 * windows in one 120-second run, paying a handshake each time, before the
 * replacement sockets failed. Four seconds still caused seven replacements
 * per source on a run that otherwise finished; an eight-second observation
 * spans the PSP Wi-Fi burst cycle and amortizes a one-second TLS handshake.
 * The floor remains below either admitted stream's average rate. The measured
 * dead connection (16 KiB in six seconds, about 2.7 KiB/s) still fails this
 * test decisively.
 */
#define MEDIA_HTTP_TRICKLE_FLOOR_BYTES_PER_SECOND 8192u
#define MEDIA_HTTP_TRICKLE_WINDOW_US 8000000u
/* Playback is already visibly blocked in this case, so waiting for the
   conservative background-rate verdict only extends the outage. */
#define MEDIA_HTTP_STARVED_NO_PROGRESS_US 2000000u
/* Enough to survive a CDN that paces the first replacement too, and few
   enough that a dead link still reaches the caller's watchdog on schedule. */
#define MEDIA_HTTP_TRICKLE_MAXIMUM_RECONNECTS 3u

static inline bool media_http_range_trickling(
    size_t gained_bytes, uint64_t elapsed_us)
{
    if (elapsed_us < MEDIA_HTTP_TRICKLE_WINDOW_US) return false;
    uint64_t floor_bytes =
        (uint64_t) MEDIA_HTTP_TRICKLE_FLOOR_BYTES_PER_SECOND
        * elapsed_us / UINT64_C(1000000);
    return (uint64_t) gained_bytes < floor_bytes;
}

static inline bool media_http_range_trickling_at_rate(
    size_t gained_bytes, uint64_t elapsed_us, size_t floor_bytes_per_second)
{
    if (elapsed_us < MEDIA_HTTP_TRICKLE_WINDOW_US) return false;
    if (floor_bytes_per_second < MEDIA_HTTP_TRICKLE_FLOOR_BYTES_PER_SECOND)
        floor_bytes_per_second = MEDIA_HTTP_TRICKLE_FLOOR_BYTES_PER_SECOND;
    uint64_t floor_bytes = (uint64_t) floor_bytes_per_second
        * elapsed_us / UINT64_C(1000000);
    return (uint64_t) gained_bytes < floor_bytes;
}

/* A completed response is retained data, not a connection which has stopped
   making progress. Keeping this decision pure makes the eight-second device
   policy testable without an eight-second host test. */
static inline bool media_http_range_should_reconnect(
    bool complete, size_t gained_bytes, uint64_t elapsed_us)
{
    return !complete
        && media_http_range_trickling(gained_bytes, elapsed_us);
}

static inline bool media_http_range_should_reconnect_at_rate(
    bool complete, size_t gained_bytes, uint64_t elapsed_us,
    size_t floor_bytes_per_second)
{
    return !complete && media_http_range_trickling_at_rate(
        gained_bytes, elapsed_us, floor_bytes_per_second);
}

static inline bool media_http_range_should_reconnect_starved(
    bool complete, bool starved, uint64_t no_progress_us)
{
    return !complete && starved
        && no_progress_us >= MEDIA_HTTP_STARVED_NO_PROGRESS_US;
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

/*
 * A single-window, bounded HTTP range source. The cache is deliberately
 * constant-size: MP4 metadata and samples are read on demand without ever
 * retaining the complete movie in the page budget.
 */
MediaHttpRange *media_http_range_create(
    Budget *budget, BrowserSession *session, const char *url,
    uint64_t content_length, const MediaHttpRangeOptions *options,
    char *error, size_t error_size);
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
/* Conservative duration represented by unread bytes in the installed
   bounded cache window plus a complete contiguous readahead window waiting
   in the scheduler. This average-bitrate estimate is for buffering UI
   hysteresis only; demux and presentation never depend on it. */
uint64_t media_http_range_buffered_ahead_us(
    const MediaHttpRange *range, uint64_t duration_us);
void media_http_range_destroy(MediaHttpRange *range);

#endif
