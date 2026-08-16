#ifndef TILEFINCH_SITE_ADAPTER_H
#define TILEFINCH_SITE_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/session.h"

/* Backward-compatible tab/config sentinel for native HOME. It is deliberately
   not handled by a site adapter and cannot produce an engine document. */
#define TILEFINCH_HOMEPAGE_URL "https://tilefinch.local/home"
#define SITE_ADAPTER_READER_CSS_LIMIT 8192u

typedef enum {
    SITE_ADAPTER_READER_FONT_SANS = 0,
    SITE_ADAPTER_READER_FONT_SERIF
} SiteAdapterReaderFont;

typedef struct SiteAdapterLoad SiteAdapterLoad;

typedef struct {
    bool youtube_compact_results;
} SiteAdapterPreferences;

typedef enum {
    SITE_ADAPTER_LOAD_PENDING = 0,
    SITE_ADAPTER_LOAD_SUCCEEDED,
    SITE_ADAPTER_LOAD_FAILED,
    SITE_ADAPTER_LOAD_CANCELLED
} SiteAdapterLoadStatus;

typedef struct {
    Budget *budget;
    char *html;
    size_t html_length;
    size_t source_bytes;
    size_t result_count;
    long status_code;
    char adapter[32];
    char server[64];
    char cf_mitigated[32];
} SiteAdapterDocument;

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
} SiteAdapterLoadMetrics;

/*
 * Site adapters are an explicit, bounded navigation boundary. BrowserEngine
 * asks this registry whether a GET is owned, then drives an opaque load job;
 * it contains no provider-specific routing or fetch logic. Returned markup is
 * browser-authored and must remain scriptless; BrowserEngine intentionally
 * commits it without allocating an author-JavaScript realm.
 */
bool site_adapter_handles_navigation(const char *method, const char *url);
/* Generic network policy for frontends that can defer transport startup.
   Unowned navigations require the ordinary network loader; an owned adapter
   declares whether its implementation needs transport. */
bool site_adapter_navigation_requires_network(
    const char *method, const char *url);
SiteAdapterLoad *site_adapter_load_begin(
    Budget *budget, BrowserSession *session, const char *method,
    const char *url, const SiteAdapterPreferences *preferences,
    size_t maximum_source_bytes, long timeout_ms,
    char *error, size_t error_size);
SiteAdapterLoadStatus site_adapter_load_pump(
    SiteAdapterLoad *load, const FetchPumpQuota *quota);
SiteAdapterLoadStatus site_adapter_load_status(const SiteAdapterLoad *load);
void site_adapter_load_cancel(SiteAdapterLoad *load, const char *reason);
bool site_adapter_load_take_document(
    SiteAdapterLoad *load, SiteAdapterDocument *document);
bool site_adapter_load_metrics(
    const SiteAdapterLoad *load, SiteAdapterLoadMetrics *metrics);
const char *site_adapter_load_error(const SiteAdapterLoad *load);
void site_adapter_load_destroy(SiteAdapterLoad *load);

bool site_adapter_load_sync(
    Budget *budget, BrowserSession *session, const char *method,
    const char *url, const SiteAdapterPreferences *preferences,
    size_t maximum_source_bytes, long timeout_ms,
    SiteAdapterDocument *document, char *error, size_t error_size);
void site_adapter_document_destroy(SiteAdapterDocument *document);

/*
 * Reader mode is a presentation adapter, not a document rewrite. It emits a
 * bounded user stylesheet for an ordinary HTTP(S) document so the caller can
 * toggle the transform off without reparsing or losing page state.
 */
bool site_adapter_reader_css(
    const char *url, SiteAdapterReaderFont font, unsigned font_percent,
    char *css, size_t capacity, char *adapter, size_t adapter_capacity);

#endif
