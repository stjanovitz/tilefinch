#ifndef TILEFINCH_YOUTUBE_LITE_H
#define TILEFINCH_YOUTUBE_LITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/session.h"

#define YOUTUBE_LITE_MAXIMUM_RESULTS 12
#define YOUTUBE_LITE_MAXIMUM_COMMENTS 8
#define YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES (2u * 1024u * 1024u)
#define YOUTUBE_LITE_MAXIMUM_COMMENTS_BYTES (256u * 1024u)
#define YOUTUBE_LITE_MAXIMUM_HTML_BYTES (96u * 1024u)

typedef enum {
    YOUTUBE_LITE_ROUTE_NONE = 0,
    YOUTUBE_LITE_ROUTE_HOME,
    YOUTUBE_LITE_ROUTE_SEARCH,
    YOUTUBE_LITE_ROUTE_WATCH,
    YOUTUBE_LITE_ROUTE_CHANNEL
} YoutubeLiteRoute;

typedef struct {
    Budget *budget;
    char *html;
    size_t html_length;
    size_t source_bytes;
    size_t result_count;
    YoutubeLiteRoute route;
    long status_code;
    char server[64];
    char cf_mitigated[32];
} YoutubeLiteDocument;

typedef struct YoutubeLiteLoadJob YoutubeLiteLoadJob;

/*
 * Small resolver context retained from the mobile page the provider already
 * fetched. Reusing this bounded BrowserSession-owned record avoids fetching
 * the same multi-megabyte watch page again when playback begins.
 */
typedef struct {
    char api_key[128];
    char visitor[1024];
} YoutubeLiteResolverIdentity;

typedef enum {
    YOUTUBE_LITE_LOAD_PENDING = 0,
    YOUTUBE_LITE_LOAD_SUCCEEDED,
    YOUTUBE_LITE_LOAD_FAILED,
    YOUTUBE_LITE_LOAD_CANCELLED
} YoutubeLiteLoadStatus;

typedef struct {
    size_t pump_calls;
    size_t network_pumps;
    size_t completion_per_mille;
    size_t body_bytes;
    size_t body_callbacks;
    size_t peak_buffered_bytes;
    size_t quota_yields;
    size_t requests_started;
    size_t requests_completed;
    size_t build_slices;
    size_t transform_quota_overruns;
    uint64_t network_us;
    uint64_t build_us;
    uint64_t maximum_pump_us;
    uint64_t maximum_transform_slice_us;
    uint64_t maximum_irreducible_unit_us;
} YoutubeLiteLoadMetrics;

/*
 * The lightweight provider is deliberately narrow. It owns only the public
 * HTTPS home, search, and playable watch routes; every other YouTube URL
 * remains an ordinary browser navigation.
 */
YoutubeLiteRoute youtube_lite_route(const char *url);

/*
 * Converts the bounded data embedded in an ordinary mobile YouTube response
 * into a small, script-free HTML document. This pure boundary is shared by
 * retained fixtures and the live loader.
 */
bool youtube_lite_build_document(
    Budget *budget, const char *url, const char *source,
    size_t source_length, YoutubeLiteDocument *document,
    char *error, size_t error_size);

/*
 * Testable form of the same pure boundary for an explicitly requested
 * comments view. comments may be NULL for a response which had no comments.
 */
bool youtube_lite_build_document_with_comments(
    Budget *budget, const char *url, const char *source,
    size_t source_length, const char *comments, size_t comments_length,
    YoutubeLiteDocument *document, char *error, size_t error_size);

/*
 * Fetches the same mobile page data used by YouTube's own UI, accepts its
 * response cookies through BrowserSession, and then invokes the pure builder.
 */
bool youtube_lite_load(
    Budget *budget, BrowserSession *session, const char *url,
    size_t maximum_source_bytes, long timeout_ms,
    YoutubeLiteDocument *document, char *error, size_t error_size);

/*
 * Cooperative form of youtube_lite_load(). Network work is advanced only by
 * pump calls and can be cancelled between them. Initial-data discovery and
 * string decoding advance in 16 KiB transform slices. Renderer discovery,
 * metadata extraction, and HTML emission also advance as bounded,
 * cancellation-safe build slices.
 */
YoutubeLiteLoadJob *youtube_lite_load_begin(
    Budget *budget, BrowserSession *session, const char *url,
    size_t maximum_source_bytes, long timeout_ms,
    char *error, size_t error_size);
YoutubeLiteLoadJob *youtube_lite_load_begin_configured(
    Budget *budget, BrowserSession *session, const char *url,
    bool compact_results, size_t maximum_source_bytes, long timeout_ms,
    char *error, size_t error_size);
YoutubeLiteLoadStatus youtube_lite_load_pump(
    YoutubeLiteLoadJob *job, const FetchPumpQuota *quota);
YoutubeLiteLoadStatus youtube_lite_load_status(
    const YoutubeLiteLoadJob *job);
void youtube_lite_load_cancel(YoutubeLiteLoadJob *job,
                              const char *reason);
bool youtube_lite_load_take_document(
    YoutubeLiteLoadJob *job, YoutubeLiteDocument *document);
bool youtube_lite_load_metrics(
    const YoutubeLiteLoadJob *job, YoutubeLiteLoadMetrics *metrics);
bool youtube_lite_resolver_identity_get(
    BrowserSession *session, YoutubeLiteResolverIdentity *identity);
const char *youtube_lite_load_error(const YoutubeLiteLoadJob *job);
void youtube_lite_load_destroy(YoutubeLiteLoadJob *job);

void youtube_lite_document_destroy(YoutubeLiteDocument *document);

#endif
