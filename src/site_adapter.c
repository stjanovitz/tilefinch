#include "tilefinch/site_adapter.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "tilefinch/content_blocker.h"
#include "tilefinch/youtube_lite.h"

typedef struct SiteAdapterDefinition SiteAdapterDefinition;

typedef void *(*SiteAdapterBeginCallback)(
    Budget *budget, BrowserSession *session, const char *url,
    const SiteAdapterPreferences *preferences,
    size_t maximum_source_bytes, long timeout_ms,
    char *error, size_t error_size);
typedef SiteAdapterLoadStatus (*SiteAdapterPumpCallback)(
    void *implementation, const FetchPumpQuota *quota);
typedef SiteAdapterLoadStatus (*SiteAdapterStatusCallback)(
    const void *implementation);
typedef void (*SiteAdapterCancelCallback)(
    void *implementation, const char *reason);
typedef bool (*SiteAdapterTakeCallback)(
    void *implementation, SiteAdapterDocument *document);
typedef bool (*SiteAdapterMetricsCallback)(
    const void *implementation, SiteAdapterLoadMetrics *metrics);
typedef const char *(*SiteAdapterErrorCallback)(const void *implementation);
typedef void (*SiteAdapterDestroyCallback)(void *implementation);

struct SiteAdapterDefinition {
    const char *name;
    bool requires_network;
    bool (*matches)(const char *url);
    SiteAdapterBeginCallback begin;
    SiteAdapterPumpCallback pump;
    SiteAdapterStatusCallback status;
    SiteAdapterCancelCallback cancel;
    SiteAdapterTakeCallback take;
    SiteAdapterMetricsCallback metrics;
    SiteAdapterErrorCallback error;
    SiteAdapterDestroyCallback destroy;
};

struct SiteAdapterLoad {
    Budget *budget;
    const SiteAdapterDefinition *definition;
    void *implementation;
};

static bool youtube_matches(const char *url)
{
    return youtube_lite_route(url) != YOUTUBE_LITE_ROUTE_NONE;
}

static void *youtube_begin(
    Budget *budget, BrowserSession *session, const char *url,
    const SiteAdapterPreferences *preferences,
    size_t maximum_source_bytes, long timeout_ms,
    char *error, size_t error_size)
{
    return youtube_lite_load_begin_configured(
        budget, session, url,
        preferences != NULL && preferences->youtube_compact_results,
        maximum_source_bytes, timeout_ms,
        error, error_size);
}

static SiteAdapterLoadStatus youtube_pump(
    void *implementation, const FetchPumpQuota *quota)
{
    YoutubeLiteLoadStatus status = youtube_lite_load_pump(
        implementation, quota);
    switch (status) {
    case YOUTUBE_LITE_LOAD_PENDING:
        return SITE_ADAPTER_LOAD_PENDING;
    case YOUTUBE_LITE_LOAD_SUCCEEDED:
        return SITE_ADAPTER_LOAD_SUCCEEDED;
    case YOUTUBE_LITE_LOAD_CANCELLED:
        return SITE_ADAPTER_LOAD_CANCELLED;
    case YOUTUBE_LITE_LOAD_FAILED:
    default:
        return SITE_ADAPTER_LOAD_FAILED;
    }
}

static SiteAdapterLoadStatus youtube_status(const void *implementation)
{
    YoutubeLiteLoadStatus status = youtube_lite_load_status(implementation);
    switch (status) {
    case YOUTUBE_LITE_LOAD_PENDING:
        return SITE_ADAPTER_LOAD_PENDING;
    case YOUTUBE_LITE_LOAD_SUCCEEDED:
        return SITE_ADAPTER_LOAD_SUCCEEDED;
    case YOUTUBE_LITE_LOAD_CANCELLED:
        return SITE_ADAPTER_LOAD_CANCELLED;
    case YOUTUBE_LITE_LOAD_FAILED:
    default:
        return SITE_ADAPTER_LOAD_FAILED;
    }
}

static void youtube_cancel(void *implementation, const char *reason)
{
    youtube_lite_load_cancel(implementation, reason);
}

static bool youtube_take(
    void *implementation, SiteAdapterDocument *document)
{
    if (document == NULL) return false;
    YoutubeLiteDocument youtube = {0};
    if (!youtube_lite_load_take_document(implementation, &youtube))
        return false;
    *document = (SiteAdapterDocument) {
        .budget = youtube.budget,
        .html = youtube.html,
        .html_length = youtube.html_length,
        .source_bytes = youtube.source_bytes,
        .result_count = youtube.result_count,
        .status_code = youtube.status_code
    };
    snprintf(document->adapter, sizeof(document->adapter), "%s",
             "youtube-lite");
    snprintf(document->server, sizeof(document->server), "%s",
             youtube.server);
    snprintf(document->cf_mitigated, sizeof(document->cf_mitigated), "%s",
             youtube.cf_mitigated);
    youtube.html = NULL;
    youtube_lite_document_destroy(&youtube);
    return true;
}

static bool youtube_metrics(
    const void *implementation, SiteAdapterLoadMetrics *metrics)
{
    if (metrics == NULL) return false;
    YoutubeLiteLoadMetrics youtube = {0};
    if (!youtube_lite_load_metrics(implementation, &youtube)) return false;
    *metrics = (SiteAdapterLoadMetrics) {
        .pump_calls = youtube.pump_calls,
        .network_pumps = youtube.network_pumps,
        .completion_per_mille = youtube.completion_per_mille,
        .body_bytes = youtube.body_bytes,
        .body_callbacks = youtube.body_callbacks,
        .peak_buffered_bytes = youtube.peak_buffered_bytes,
        .quota_yields = youtube.quota_yields,
        .requests_started = youtube.requests_started,
        .requests_completed = youtube.requests_completed,
        .build_slices = youtube.build_slices,
        .transform_quota_overruns =
            youtube.transform_quota_overruns,
        .network_us = youtube.network_us,
        .build_us = youtube.build_us,
        .maximum_pump_us = youtube.maximum_pump_us,
        .maximum_transform_slice_us =
            youtube.maximum_transform_slice_us,
        .maximum_irreducible_unit_us =
            youtube.maximum_irreducible_unit_us
    };
    return true;
}

static const char *youtube_error(const void *implementation)
{
    return youtube_lite_load_error(implementation);
}

static void youtube_destroy(void *implementation)
{
    youtube_lite_load_destroy(implementation);
}

static const SiteAdapterDefinition adapters[] = {
    {
        .name = "youtube-lite",
        .requires_network = true,
        .matches = youtube_matches,
        .begin = youtube_begin,
        .pump = youtube_pump,
        .status = youtube_status,
        .cancel = youtube_cancel,
        .take = youtube_take,
        .metrics = youtube_metrics,
        .error = youtube_error,
        .destroy = youtube_destroy
    }
};

static const SiteAdapterDefinition *site_adapter_find(
    const char *method, const char *url)
{
    if (method == NULL || url == NULL || strcasecmp(method, "GET") != 0)
        return NULL;
    for (size_t i = 0; i < sizeof(adapters) / sizeof(adapters[0]); i++) {
        if (adapters[i].matches(url)) return &adapters[i];
    }
    return NULL;
}

bool site_adapter_handles_navigation(const char *method, const char *url)
{
    return site_adapter_find(method, url) != NULL;
}

bool site_adapter_navigation_requires_network(
    const char *method, const char *url)
{
    const SiteAdapterDefinition *definition =
        site_adapter_find(method, url);
    return definition == NULL || definition->requires_network;
}

SiteAdapterLoad *site_adapter_load_begin(
    Budget *budget, BrowserSession *session, const char *method,
    const char *url, const SiteAdapterPreferences *preferences,
    size_t maximum_source_bytes, long timeout_ms,
    char *error, size_t error_size)
{
    if (error != NULL && error_size != 0) error[0] = '\0';
    const SiteAdapterDefinition *definition =
        site_adapter_find(method, url);
    if (budget == NULL || session == NULL || definition == NULL) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "%s", "no site adapter owns URL");
        return NULL;
    }
    SiteAdapterLoad *load = budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, 1, sizeof(*load));
    if (load == NULL) {
        if (error != NULL && error_size != 0)
            snprintf(error, error_size, "%s",
                     "site adapter exceeded its memory bound");
        return NULL;
    }
    load->budget = budget;
    load->definition = definition;
    load->implementation = definition->begin(
        budget, session, url, preferences, maximum_source_bytes, timeout_ms,
        error, error_size);
    if (load->implementation == NULL) {
        budget_free(budget, load);
        return NULL;
    }
    return load;
}

SiteAdapterLoadStatus site_adapter_load_pump(
    SiteAdapterLoad *load, const FetchPumpQuota *quota)
{
    return load == NULL || load->definition == NULL
        ? SITE_ADAPTER_LOAD_FAILED
        : load->definition->pump(load->implementation, quota);
}

SiteAdapterLoadStatus site_adapter_load_status(const SiteAdapterLoad *load)
{
    return load == NULL || load->definition == NULL
        ? SITE_ADAPTER_LOAD_FAILED
        : load->definition->status(load->implementation);
}

void site_adapter_load_cancel(SiteAdapterLoad *load, const char *reason)
{
    if (load != NULL && load->definition != NULL)
        load->definition->cancel(load->implementation, reason);
}

bool site_adapter_load_take_document(
    SiteAdapterLoad *load, SiteAdapterDocument *document)
{
    return load != NULL && load->definition != NULL
        && load->definition->take(load->implementation, document);
}

bool site_adapter_load_metrics(
    const SiteAdapterLoad *load, SiteAdapterLoadMetrics *metrics)
{
    return load != NULL && load->definition != NULL
        && load->definition->metrics(load->implementation, metrics);
}

const char *site_adapter_load_error(const SiteAdapterLoad *load)
{
    return load == NULL || load->definition == NULL
        ? "site adapter load is null"
        : load->definition->error(load->implementation);
}

void site_adapter_load_destroy(SiteAdapterLoad *load)
{
    if (load == NULL) return;
    Budget *budget = load->budget;
    if (load->definition != NULL)
        load->definition->destroy(load->implementation);
    budget_free(budget, load);
}

bool site_adapter_load_sync(
    Budget *budget, BrowserSession *session, const char *method,
    const char *url, const SiteAdapterPreferences *preferences,
    size_t maximum_source_bytes, long timeout_ms,
    SiteAdapterDocument *document, char *error, size_t error_size)
{
#ifdef __PSP__
    (void) budget; (void) session; (void) method; (void) url;
    (void) preferences; (void) maximum_source_bytes; (void) timeout_ms;
    if (document != NULL) *document = (SiteAdapterDocument) {0};
    if (error != NULL && error_size != 0) {
        snprintf(
            error, error_size,
            "synchronous site adapters are disabled on PSP");
    }
    return false;
#else
    if (document == NULL) return false;
    *document = (SiteAdapterDocument) {0};
    SiteAdapterLoad *load = site_adapter_load_begin(
        budget, session, method, url, preferences,
        maximum_source_bytes, timeout_ms,
        error, error_size);
    if (load == NULL) return false;
    SiteAdapterLoadStatus status = SITE_ADAPTER_LOAD_PENDING;
    while (status == SITE_ADAPTER_LOAD_PENDING)
        status = site_adapter_load_pump(load, NULL);
    bool loaded = status == SITE_ADAPTER_LOAD_SUCCEEDED
        && site_adapter_load_take_document(load, document);
    if (!loaded && error != NULL && error_size != 0) {
        snprintf(error, error_size, "%s", site_adapter_load_error(load));
    }
    site_adapter_load_destroy(load);
    return loaded;
#endif
}

void site_adapter_document_destroy(SiteAdapterDocument *document)
{
    if (document == NULL) return;
    if (document->budget != NULL && document->html != NULL)
        budget_free(document->budget, document->html);
    *document = (SiteAdapterDocument) {0};
}

static bool reader_append(char *output, size_t capacity, size_t *used,
                          const char *fragment)
{
    if (output == NULL || used == NULL || fragment == NULL
        || *used >= capacity) return false;
    size_t length = strlen(fragment);
    if (length >= capacity - *used) return false;
    memcpy(output + *used, fragment, length);
    *used += length;
    output[*used] = '\0';
    return true;
}

bool site_adapter_reader_css(
    const char *url, SiteAdapterReaderFont font, unsigned font_percent,
    char *css, size_t capacity, char *adapter, size_t adapter_capacity)
{
    if (css == NULL || capacity == 0 || adapter == NULL
        || adapter_capacity == 0
        || (font != SITE_ADAPTER_READER_FONT_SANS
            && font != SITE_ADAPTER_READER_FONT_SERIF)
        || (font_percent != 80u && font_percent != 100u
            && font_percent != 125u && font_percent != 150u)) {
        return false;
    }
    css[0] = '\0';
    adapter[0] = '\0';
    char site[CONTENT_BLOCKER_HOST_LIMIT];
    if (!content_blocker_site_from_url(url, site)) return false;
    size_t site_length = strlen(site);
    if (strcmp(site, "localhost") == 0
        || (site_length >= 6u
            && strcmp(site + site_length - 6u, ".local") == 0)) {
        return false;
    }

    const char *family = font == SITE_ADAPTER_READER_FONT_SERIF
        ? "serif" : "sans-serif";
    char base[1536];
    int length = snprintf(
        base, sizeof(base),
        "html{font-size:%u%%!important;background:transparent!important}"
        "body{box-sizing:border-box!important;width:100%%!important;"
        "min-width:0!important;max-width:none!important;margin:0!important;"
        "padding:14px 16px!important;font-family:%s!important;"
        "line-height:1.55!important}"
        "main,article,[role=main],h1,h2,h3,h4,h5,h6,p,p *,li,li *,"
        "blockquote,blockquote *{font-family:%s!important}"
        "pre,code,kbd,samp,pre *{font-family:monospace!important}"
        "body>header,body>footer,body>nav,body>aside,"
        "[role=banner],[role=navigation],[role=complementary],"
        "[aria-label*=navigation i],[aria-label*=sidebar i]{display:none!important}"
        "body:has(article)>*:not(article):not(main):not(:has(article)),"
        "body:not(:has(article)):has(main)>*:not(main):not(:has(main))"
        "{display:none!important}"
        "main,article,[role=main],#content,.content,.main-content{"
        "box-sizing:border-box!important;display:block!important;"
        "float:none!important;position:static!important;left:auto!important;"
        "right:auto!important;width:100%%!important;min-width:0!important;"
        "max-width:none!important;margin:0!important;padding:0!important}"
        "main *,article *,[role=main] *{max-width:100%%}"
        "p,li,blockquote{font-size:1rem!important;line-height:1.55!important}"
        "img,video,svg,canvas{max-width:100%%!important;height:auto!important}"
        "pre{max-width:100%%!important;overflow:auto!important;white-space:pre-wrap!important}"
        "table{max-width:100%%!important}blockquote{margin-left:12px!important;"
        "padding-left:10px!important}",
        font_percent, family, family);
    if (length < 0 || (size_t) length >= sizeof(base)) return false;
    size_t used = 0;
    if (!reader_append(css, capacity, &used, base)) return false;

    const char *specific = "";
    const char *name = "reader-generic";
    if (strcmp(site, "wikipedia.org") == 0) {
        name = "reader-wikipedia";
        specific =
            ".header-container,.mw-header,.vector-header-container,.vector-column-start,"
            ".vector-column-end,.mw-footer,.page-actions-menu,"
            ".minerva__tab-container,.mw-editsection,.navbox,.metadata,"
            ".infobox-above{display:none!important}"
            "#content,.mw-body,.mw-body-content,.mw-parser-output{"
            "width:100%!important;max-width:none!important;margin:0!important;"
            "padding:0!important;float:none!important}";
    } else if (strcmp(site, "reddit.com") == 0) {
        name = "reader-reddit";
        specific =
            "#header,.side,.footer-parent,.promoted,.rank,.midcol,"
            ".listing-chooser{display:none!important}"
            ".content,.sitetable,.thing,.entry{width:100%!important;"
            "max-width:none!important;margin:0!important;float:none!important}";
    } else if (strcmp(site, "nytimes.com") == 0) {
        name = "reader-nytimes";
        specific =
            "header,nav,aside,footer,[data-testid*=ad],"
            "[data-testid*=newsletter]{display:none!important}"
            "#site-content,main,article{width:100%!important;max-width:none!important;"
            "margin:0!important;padding:0!important}";
    }
    if (!reader_append(css, capacity, &used, specific)) return false;
    snprintf(adapter, adapter_capacity, "%s", name);
    return true;
}
