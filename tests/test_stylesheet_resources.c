#include "tilefinch/budget.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/document.h"
#include "tilefinch/fetch.h"
#include "tilefinch/platform.h"
#include "tilefinch/resources.h"
#include "tilefinch/resource_integrity.h"
#include "tilefinch/style.h"
#include "tilefinch/user_agent.h"
#include "../src/style_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TILEFINCH_TEST_SOURCE_DIR
#define TILEFINCH_TEST_SOURCE_DIR "."
#endif

#define MIB (1024u * 1024u)

static lxb_dom_node_t *find_element_id(
    lxb_dom_node_t *node, const char *wanted)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && length == strlen(wanted)
            && memcmp(id, wanted, length) == 0) return node;
        lxb_dom_node_t *nested = find_element_id(node->first_child, wanted);
        if (nested != NULL) return nested;
    }
    return NULL;
}

static lxb_dom_node_t *find_element_href(
    lxb_dom_node_t *node, const char *wanted)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *href = document_attribute(node, "href", &length);
        if (href != NULL && length == strlen(wanted)
            && memcmp(href, wanted, length) == 0) return node;
        lxb_dom_node_t *nested = find_element_href(node->first_child, wanted);
        if (nested != NULL) return nested;
    }
    return NULL;
}

static bool begin_layout_css_replay(void)
{
    char error[256] = {0};
    return fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-section-external-layout",
        error, sizeof(error));
}

static bool test_nonmatching_media_settles_without_applying(void)
{
    static const char html[] =
        "<!doctype html><link rel=stylesheet "
        "media='screen and (min-width: 768px)' "
        "href='https://fixture.test/layout.css'>"
        "<body class=external-flex>media</body>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    ExternalStylesheetStats stats = {0};
    bool replaying = false;
    bool ok = installed
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    size_t rules_before = stylesheet.count;
    if (ok) {
        replaying = begin_layout_css_replay();
        ok = replaying && stylesheets_load_external_tracked(
            &document, &stylesheet, &budget, "https://fixture.test/page",
            4, 4096, 4096, 1000, NULL, NULL, &resources, &stats);
    }
    ok = ok && stats.discovered == 1 && stats.attempted == 1
        && stats.loaded == 1 && stats.failed == 0
        && stats.skipped_media == 1 && stats.rules_added == 0
        && stylesheet.count == rules_before && resources.count == 1
        && resources.items[0].state == STYLESHEET_DOCUMENT_RESOURCE_LOADED
        && resources.items[0].body != NULL
        && stylesheet_document_resources_link_state(
               &resources, "https://fixture.test/page",
               "https://fixture.test/layout.css",
               strlen("https://fixture.test/layout.css"))
               == STYLESHEET_DOCUMENT_RESOURCE_LOADED;
    if (replaying) fetch_trace_end();
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_integrity_mismatch_rejects_stylesheet(void)
{
    static const char html[] =
        "<!doctype html><link rel=stylesheet "
        "integrity='sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=' "
        "href='https://fixture.test/layout.css'>"
        "<body class=external-flex>integrity</body>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    ExternalStylesheetStats stats = {0};
    bool replaying = false;
    bool ok = installed
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    size_t rules_before = stylesheet.count;
    if (ok) {
        replaying = begin_layout_css_replay();
        ok = replaying && stylesheets_load_external_tracked(
            &document, &stylesheet, &budget, "https://fixture.test/page",
            4, 4096, 4096, 1000, NULL, NULL, &resources, &stats);
    }
    ok = ok && stats.discovered == 1 && stats.attempted == 1
        && stats.loaded == 0 && stats.failed == 1
        && stylesheet.count == rules_before;
    if (replaying) fetch_trace_end();
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_integrity_is_scoped_to_each_link_element(void);

static bool test_matching_duplicate_promotes_queued_response(void)
{
    static const char html[] =
        "<!doctype html>"
        "<link rel=stylesheet media='(min-width: 768px)' "
        "href='https://fixture.test/layout.css'>"
        "<link rel=stylesheet media=screen "
        "href='https://fixture.test/layout.css'>"
        "<body class=external-flex>promoted</body>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    ExternalStylesheetStats stats = {0};
    bool replaying = false;
    bool ok = installed
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    size_t rules_before = stylesheet.count;
    if (ok) {
        replaying = begin_layout_css_replay();
        ok = replaying && stylesheets_load_external_tracked(
            &document, &stylesheet, &budget, "https://fixture.test/page",
            4, 4096, 4096, 1000, NULL, NULL, &resources, &stats);
    }
    ok = ok && stats.discovered == 2 && stats.attempted == 1
        && stats.loaded == 1 && stats.failed == 0
        && stats.skipped_media == 1 && stats.duplicate == 1
        && stats.rules_added == 1 && stylesheet.count == rules_before + 1
        && resources.count == 1
        && resources.items[0].state == STYLESHEET_DOCUMENT_RESOURCE_LOADED;
    if (replaying) fetch_trace_end();
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_inactive_declared_themes_do_not_consume_quota(void)
{
    static const char html[] =
        "<!doctype html><html data-color-mode=dark data-dark-theme=dark "
        "data-color-mode=light data-light-theme=light data-dark-theme=dark>"
        "<head>"
        "<link rel=stylesheet href='https://fixture.test/light.css'>"
        "<link rel=stylesheet href='https://fixture.test/layout.css'>"
        "<link rel=stylesheet data-color-theme=light "
        "data-href='https://fixture.test/light.css'>"
        "<link rel=stylesheet data-color-theme=dark "
        "data-href='https://fixture.test/layout.css'>"
        "</head><body id=probe class=external-flex>theme</body></html>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    ExternalStylesheetStats stats = {0};
    bool replaying = false;
    bool ok = installed
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    if (ok) {
        replaying = begin_layout_css_replay();
        ok = replaying && stylesheets_load_external_tracked(
            &document, &stylesheet, &budget, "https://fixture.test/page",
            1, 4096, 4096, 1000, NULL, NULL, &resources, &stats);
    }
    lxb_dom_node_t *probe = ok ? find_element_id(
        lxb_dom_interface_node(document.html), "probe") : NULL;
    ComputedStyle style = probe == NULL ? (ComputedStyle) {0}
        : style_for_node(&stylesheet, probe, NULL);
    ok = ok && stats.discovered == 4 && stats.attempted == 1
        && stats.loaded == 1 && stats.failed == 0
        && stats.skipped_alternate_theme == 3
        && style.display == DISPLAY_FLEX
        && resources.alternate_theme_selection_valid
        && resources.alternate_theme_count == 2;
    /* Parser checkpoints append only the new source-order nodes and cannot
       rescan the earlier registry. The page-lifetime decision must still
       suppress an inactive href before it consumes a fetch/quota slot. */
    ExternalStylesheetStats suffix_stats = {0};
    lxb_dom_node_t *inactive = ok ? find_element_href(
        lxb_dom_interface_node(document.html),
        "https://fixture.test/light.css") : NULL;
    FetchScheduler *scheduler = ok
        ? fetch_scheduler_create(&budget, 1, 4096) : NULL;
    lxb_dom_node_t *suffix[1] = {inactive};
    ok = ok && inactive != NULL && scheduler != NULL
        && stylesheets_append_ordered_suffix_with_context(
               &stylesheet, &budget, suffix, 1,
               "https://fixture.test/page", "https://fixture.test/page",
               "", &document.content_security_policy, 1, 4096, 4096, 1000,
               scheduler, NULL, &resources, &suffix_stats)
        && suffix_stats.discovered == 1
        && suffix_stats.attempted == 0
        && suffix_stats.skipped_alternate_theme == 1;
    fetch_scheduler_destroy(scheduler);
    if (replaying) fetch_trace_end();
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool write_http_error_replay(char *directory, size_t capacity)
{
    static const char body[] = "body{color:red}";
    if (snprintf(directory, capacity,
                 "/tmp/tilefinch-stylesheet-http-%ld", (long) getpid())
            < 0
        || mkdir(directory, 0700) != 0) return false;
    char body_path[512];
    char meta_path[512];
    char clock_path[512];
    if (snprintf(body_path, sizeof(body_path), "%s/0000.body", directory)
            < 0
        || snprintf(meta_path, sizeof(meta_path), "%s/0000.meta", directory)
            < 0
        || snprintf(clock_path, sizeof(clock_path), "%s/trace.meta", directory)
            < 0) return false;
    FILE *body_file = fopen(body_path, "wb");
    bool ok = false;
    if (body_file != NULL) {
        bool wrote = fwrite(body, 1, sizeof(body) - 1u, body_file)
                     == sizeof(body) - 1u;
        bool closed = fclose(body_file) == 0;
        ok = wrote && closed;
    }
    FILE *meta = ok ? fopen(meta_path, "wb") : NULL;
    if (meta == NULL) return false;
    bool meta_written = fprintf(
        meta,
        "psp-http-trace=3\n"
        "method=GET\n"
        "url=https://fixture.test/missing.css\n"
        "success=1\n"
        "async-delay-pumps=1\n"
        "external-cancel=0\n"
        "transport-timeout=0\n"
        "error=\n"
        "request-body-length=0\n"
        "request-body-hash=cbf29ce484222325\n"
        "request-send-client-hints=0\n"
        "request-send-low-client-hints=0\n"
        "request-sec-fetch-user=0\n"
        "request-upgrade-insecure=0\n"
        "status=404\n"
        "length=%zu\n"
        "effective-url=https://fixture.test/missing.css\n"
        "content-type=text/css\n"
        "etag=\nlast-modified=\ncf-mitigated=\naccept-ch=\ncritical-ch=\n"
        "server=fixture\ncf-ray=\n"
        "response-header-count=1\nset-cookie-count=0\n"
        "response-header-0=content-type: text/css\n",
        sizeof(body) - 1u) > 0;
    bool meta_closed = fclose(meta) == 0;
    ok = meta_written && meta_closed;
    if (!ok) return false;
    FILE *clock = fopen(clock_path, "wb");
    if (clock == NULL) return false;
    bool clock_written = fprintf(
        clock, "psp-http-trace-clock=1\norigin-ms=1000\n") > 0;
    bool clock_closed = fclose(clock) == 0;
    return clock_written && clock_closed;
}

static void remove_http_error_replay(const char *directory)
{
    char path[512];
    const char *files[] = {"0000.body", "0000.meta", "trace.meta"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (snprintf(path, sizeof(path), "%s/%s", directory, files[i]) > 0) {
            (void) unlink(path);
        }
    }
    (void) rmdir(directory);
}

typedef struct {
    const char *url;
    const char *body;
    const char *effective_url;
    const char *content_type;
    const char *request_accept;
    const char *request_sec_fetch_dest;
    const char *request_sec_fetch_mode;
    const char *request_referer;
    const char *request_sec_fetch_site;
    const char *request_credential_origin;
    const char *request_initiator_url;
    const char *request_referrer_source;
    const char *request_referrer_policy;
    const char *request_if_none_match;
    const char *request_if_modified_since;
    const char *response_referrer_policy;
    const char *response_cache_control;
    size_t request_cookie_bytes;
    long status;
    bool request_send_low_client_hints;
} StylesheetReplayRecord;

static uint64_t stylesheet_replay_body_hash(const char *data, size_t length)
{
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++) {
        value ^= (unsigned char) data[i];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static bool write_stylesheet_replay_record(
    FILE *meta, const StylesheetReplayRecord *record, size_t length)
{
    if (meta == NULL || record == NULL || record->url == NULL
        || record->body == NULL || record->effective_url == NULL
        || record->request_sec_fetch_site == NULL
        || record->request_credential_origin == NULL
        || record->request_initiator_url == NULL
        || record->request_referrer_source == NULL
        || record->request_referrer_policy == NULL) return false;
    const char *content_type = record->content_type == NULL
        ? "text/css" : record->content_type;
    const char *request_accept = record->request_accept == NULL
        ? "text/css,*/*;q=0.1" : record->request_accept;
    const char *request_destination = record->request_sec_fetch_dest == NULL
        ? "style" : record->request_sec_fetch_dest;
    const char *request_mode = record->request_sec_fetch_mode == NULL
        ? "no-cors" : record->request_sec_fetch_mode;
    size_t header_count = 1u
        + (record->response_referrer_policy == NULL ? 0u : 1u)
        + (record->response_cache_control == NULL ? 0u : 1u);
    bool ok = fprintf(
        meta,
        "psp-http-trace=10\n"
        "cookie-values=redacted\n"
        "method=GET\nurl=%s\nlogical-request-url=%s\nsuccess=1\n"
        "async-delay-pumps=0\nexternal-cancel=0\n"
        "transport-timeout=0\nredirect-origin-tainted=0\nerror=\n"
        "request-body-length=0\n"
        "request-body-hash=cbf29ce484222325\n"
        "request-content-type=\n"
        "request-cookie-bytes=%zu\nrequest-has-cf-clearance=0\n"
        "request-extra-header-bytes=0\nrequest-extra-header-shape=\n"
        "request-allow-http-errors=0\nrequest-enforce-cors=0\n"
        "request-redirect-same-origin-only=0\n"
        "request-cors-cached-response-validated=0\n"
        "request-if-none-match=%s\nrequest-if-modified-since=%s\n"
        "request-referer=%s\nrequest-origin=\n"
        "request-accept=%s\n"
        "request-sec-fetch-dest=%s\n"
        "request-sec-fetch-mode=%s\n"
        "request-sec-fetch-site=%s\n"
        "request-send-client-hints=0\nrequest-client-hint-tokens=\n"
        "request-client-hint-origin=\n"
        "request-send-low-client-hints=%d\n"
        "request-sec-fetch-user=0\nrequest-upgrade-insecure=0\n"
        "request-user-agent=" TILEFINCH_BROWSER_USER_AGENT "\n"
        "request-diagnostic-mobile-safari=0\nrequest-credentials=0\n"
        "request-credential-origin=%s\nrequest-initiator-url=%s\n"
        "request-referrer-source=%s\nrequest-referrer-policy=%s\n"
        "status=%ld\nlength=%zu\nresponse-body-hash=%016llx\n"
        "effective-url=%s\ncontent-type=%s\netag=\n"
        "last-modified=\ncf-mitigated=\naccept-ch=\ncritical-ch=\n"
        "server=fixture\ncf-ray=\n"
        "response-referrer-policy-metadata-valid=1\n"
        "response-referrer-policy-present=%d\n"
        "response-referrer-policy=%s\n"
        "response-security-headers-truncated=0\n"
        "response-header-count=%zu\n"
        "set-cookie-count=0\nresponse-header-0=content-type: %s\n",
        record->url, record->url,
        record->request_cookie_bytes,
        record->request_if_none_match == NULL
            ? "" : record->request_if_none_match,
        record->request_if_modified_since == NULL
            ? "" : record->request_if_modified_since,
        record->request_referer == NULL ? "" : record->request_referer,
        request_accept, request_destination, request_mode,
        record->request_sec_fetch_site,
        record->request_send_low_client_hints ? 1 : 0,
        record->request_credential_origin,
        record->request_initiator_url,
        record->request_referrer_source,
        record->request_referrer_policy,
        record->status, length,
        (unsigned long long) stylesheet_replay_body_hash(
            record->body, length),
        record->effective_url, content_type,
        record->response_referrer_policy == NULL ? 0 : 1,
        record->response_referrer_policy == NULL
            ? "" : record->response_referrer_policy,
        header_count,
        content_type) > 0;
    size_t header_index = 1;
    if (ok && record->response_referrer_policy != NULL) {
        ok = fprintf(meta, "response-header-%zu=referrer-policy: %s\n",
                     header_index++,
                     record->response_referrer_policy) > 0;
    }
    if (ok && record->response_cache_control != NULL) {
        ok = fprintf(meta, "response-header-%zu=cache-control: %s\n",
                     header_index, record->response_cache_control) > 0;
    }
    return ok;
}

static bool write_stylesheet_replay(
    char directory[128], const StylesheetReplayRecord *records, size_t count)
{
    snprintf(directory, 128, "%s", "/tmp/tilefinch-stylesheet-XXXXXX");
    if (records == NULL || count == 0 || mkdtemp(directory) == NULL) {
        return false;
    }
    bool ok = true;
    for (size_t i = 0; ok && i < count; i++) {
        char body_path[192], meta_path[192];
        ok = snprintf(body_path, sizeof(body_path), "%s/%04zu.body",
                      directory, i) > 0
            && snprintf(meta_path, sizeof(meta_path), "%s/%04zu.meta",
                        directory, i) > 0;
        size_t length = strlen(records[i].body);
        FILE *body = ok ? fopen(body_path, "wb") : NULL;
        bool body_written = body != NULL
            && fwrite(records[i].body, 1, length, body) == length;
        bool body_closed = body != NULL && fclose(body) == 0;
        if (!body_written || !body_closed) {
            ok = false;
            break;
        }
        FILE *meta = fopen(meta_path, "wb");
        if (meta == NULL) {
            ok = false;
            break;
        }
        bool written = write_stylesheet_replay_record(
            meta, &records[i], length);
        ok = fclose(meta) == 0 && written;
    }
    char clock_path[192];
    if (ok && snprintf(clock_path, sizeof(clock_path), "%s/trace.meta",
                       directory) > 0) {
        FILE *clock = fopen(clock_path, "wb");
        ok = clock != NULL
            && fprintf(clock,
                       "psp-http-trace-clock=1\norigin-ms=1000\n") > 0
            && fclose(clock) == 0;
    } else {
        ok = false;
    }
    return ok;
}

static void remove_stylesheet_replay(const char *directory, size_t count)
{
    char path[192];
    for (size_t i = 0; i < count; i++) {
        if (snprintf(path, sizeof(path), "%s/%04zu.body", directory, i)
                > 0) (void) unlink(path);
        if (snprintf(path, sizeof(path), "%s/%04zu.meta", directory, i)
                > 0) (void) unlink(path);
    }
    if (snprintf(path, sizeof(path), "%s/trace.meta", directory) > 0) {
        (void) unlink(path);
    }
    (void) rmdir(directory);
}

static bool test_integrity_rechecks_cross_origin_redirect(void)
{
    static const char document_url[] = "https://document.example/page";
    static const char request_url[] = "https://document.example/site.css";
    static const char final_url[] = "https://cross-origin.example/site.css";
    static const char html[] =
        "<!doctype html><link rel=stylesheet "
        "href='https://document.example/site.css' "
        "integrity='sha256-FcQqt3aNlV7AZnGV4zkQRVeCeJOxbMPnQSx258L803E='>";
    static const StylesheetReplayRecord replay[] = {{
        .url = request_url, .body = "body{color:red}",
        .effective_url = final_url, .request_referer = document_url,
        .request_sec_fetch_site = "same-origin",
        .request_credential_origin = document_url,
        .request_initiator_url = document_url,
        .request_referrer_source = document_url,
        .request_referrer_policy = ""
    }};
    char replay_directory[128] = {0};
    bool replay_written = write_stylesheet_replay(
        replay_directory, replay, 1);
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    ExternalStylesheetStats stats = {0};
    bool replaying = false;
    bool ok = replay_written && installed
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    size_t rules_before = stylesheet.count;
    if (ok) {
        char error[256] = {0};
        replaying = fetch_trace_replay_begin(
            replay_directory, error, sizeof(error));
        ok = replaying && stylesheets_load_external_tracked(
            &document, &stylesheet, &budget, document_url,
            4, 4096, 4096, 1000, NULL, NULL, &resources, &stats);
    }
    ok = ok && stats.attempted == 1 && stats.loaded == 0
        && stats.failed == 1 && stylesheet.count == rules_before;
    if (replaying) fetch_trace_end();
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    if (replay_written) remove_stylesheet_replay(replay_directory, 1);
    return ok && clean;
}

static bool test_integrity_is_scoped_to_each_link_element(void)
{
    static const char url[] = "https://fixture.test/layout.css";
    static const char page[] = "https://fixture.test/page";
    static const char body[] = ".external-flex { display: flex; }\n";
    static const char html[] =
        "<!doctype html>"
        "<link rel=stylesheet href='https://fixture.test/layout.css' "
        "integrity='sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA='>"
        "<link rel=stylesheet href='https://fixture.test/layout.css' "
        "integrity='sha256-tRqapj4TB07ICTf060V3hrmSeXqDEIrl7pj6gge6rzY='>"
        "<body class=external-flex>element scoped integrity</body>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession session = {0};
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    ExternalStylesheetStats stats = {0};
    unsigned char *cached_copy = budget_malloc(&budget, sizeof(body) - 1u);
    if (cached_copy != NULL) {
        memcpy(cached_copy, body, sizeof(body) - 1u);
    }
    BrowserSharedBody *cached_body = browser_shared_body_take(
        &budget, cached_copy, sizeof(body) - 1u);
    if (cached_body == NULL) budget_free(&budget, cached_copy);
    TilefinchRequestContext cached_context = {
        .target_url = url,
        .initiator_url = page,
        .top_level_url = page,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_STYLE
    };
    TilefinchResourceGrant cached_grant = {
        .destination = TILEFINCH_DESTINATION_STYLE,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .final_same_origin = true,
        .final_same_site = true,
        .cors_validated = true
    };
    bool ok = installed
        && browser_session_init(&session, &budget, 512u * 1024u)
        && cached_body != NULL
        && browser_session_cache_put_http_shared_resource(
               &session, url, cached_body, NULL, NULL, "text/css",
               "max-age=3600", NULL,
               tilefinch_platform_monotonic_time_ns(), &cached_context,
               &cached_grant)
        && browser_session_cache_set_resource_response_provenance(
               &session, url, &cached_context, url, "")
        && tilefinch_resource_integrity_verify(
               "sha256-tRqapj4TB07ICTf060V3hrmSeXqDEIrl7pj6gge6rzY=",
               strlen("sha256-tRqapj4TB07ICTf060V3hrmSeXqDEIrl7pj6gge6rzY="),
               (const uint8_t *) body, sizeof(body) - 1u)
               == TILEFINCH_INTEGRITY_MATCH
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    browser_shared_body_release(cached_body);
    size_t rules_before = stylesheet.count;
    if (ok) ok = stylesheets_load_external_tracked_with_context(
        &document, &stylesheet, &budget, page, page, "", 4, 4096,
        4096, 1000, NULL, &session, &resources, &stats);
    if (!(ok && stats.discovered == 2 && stats.attempted == 2
          && stats.loaded == 1 && stats.failed == 1
          && stylesheet.count > rules_before)) {
        fprintf(stderr,
                "element SRI stats ok=%d discovered=%zu attempted=%zu "
                "loaded=%zu failed=%zu rules=%zu/%zu duplicate=%zu\n",
                ok, stats.discovered, stats.attempted, stats.loaded,
                stats.failed, stylesheet.count, rules_before,
                stats.duplicate);
    }
    ok = ok && stats.discovered == 2 && stats.attempted == 2
        && stats.loaded == 1 && stats.failed == 1
        && stylesheet.count > rules_before;
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    browser_session_destroy(&session);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static const StylesheetDocumentResource *find_stylesheet_resource(
    const StylesheetDocumentResources *resources, const char *url)
{
    if (resources == NULL || url == NULL) return NULL;
    for (size_t i = 0; i < resources->count; i++) {
        if (resources->items[i].url != NULL
            && strcmp(resources->items[i].url, url) == 0) {
            return &resources->items[i];
        }
    }
    return NULL;
}

static bool test_initial_link_uses_document_context_and_attribute_policy(void)
{
    static const char html[] =
        "<!doctype html><link rel=stylesheet href='site.css' "
        "referrerpolicy='NO-REFERRER'><body>context</body>";
    static const char base_url[] = "https://assets.example/theme/";
    static const char document_url[] =
        "https://document.example/articles/page.html";
    static const char stylesheet_url[] =
        "https://assets.example/theme/site.css";
    static const StylesheetReplayRecord replay[] = {{
        .url = stylesheet_url,
        .body = "body{color:#123456}",
        .effective_url = stylesheet_url,
        .request_sec_fetch_site = "cross-site",
        .request_credential_origin = document_url,
        .request_initiator_url = document_url,
        .request_referrer_source = document_url,
        .request_referrer_policy = "no-referrer",
        .response_cache_control = "max-age=3600",
        .status = 200
    }};
    char replay_directory[128] = {0};
    bool replay_written = write_stylesheet_replay(
        replay_directory, replay, sizeof(replay) / sizeof(replay[0]));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession session = {0};
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    ExternalStylesheetStats stats = {0};
    bool replaying = false;
    bool ok = replay_written && installed
        && browser_session_init(&session, &budget, 512u * 1024u)
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    if (ok) {
        char error[256] = {0};
        replaying = fetch_trace_replay_begin(
            replay_directory, error, sizeof(error));
        ok = replaying && stylesheets_load_external_tracked_with_context(
            &document, &stylesheet, &budget, base_url, document_url,
            "unsafe-url", 4, 4096, 4096, 1000, NULL, &session,
            &resources, &stats);
    }
    const StylesheetDocumentResource *resource = find_stylesheet_resource(
        &resources, stylesheet_url);
    ok = ok && stats.attempted == 1 && stats.loaded == 1
        && stats.cache_hits == 0 && resource != NULL
        && resource->response_provenance_known
        && resource->response_referrer_policy[0] == '\0';
    if (replaying) fetch_trace_end();
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    browser_session_destroy(&session);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    if (replay_written) remove_stylesheet_replay(
        replay_directory, sizeof(replay) / sizeof(replay[0]));
    return ok && clean;
}

static bool test_author_rules_follow_document_input_order(void)
{
    static const char document_url[] =
        "https://document.example/article/page.html";
    static const char stylesheet_url[] =
        "https://document.example/article/middle.css";
    static const char html[] =
        "<!doctype html><style>.target{color:#110000}</style>"
        "<link rel=stylesheet href=middle.css>"
        "<style>.target{color:#003300}</style>"
        "<body><div id=target class=target>ordered</div></body>";
    static const StylesheetReplayRecord replay[] = {{
        .url = stylesheet_url,
        .body = ".target{color:#000022}",
        .effective_url = stylesheet_url,
        .request_referer = document_url,
        .request_sec_fetch_site = "same-origin",
        .request_credential_origin = document_url,
        .request_initiator_url = document_url,
        .request_referrer_source = document_url,
        .request_referrer_policy = "unsafe-url",
        .response_cache_control = "max-age=3600",
        .status = 200
    }};
    char replay_directory[128] = {0};
    bool replay_written = write_stylesheet_replay(
        replay_directory, replay, sizeof(replay) / sizeof(replay[0]));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    ExternalStylesheetStats stats = {0};
    bool replaying = false;
    bool ok = replay_written && installed
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    if (ok) {
        char error[256] = {0};
        replaying = fetch_trace_replay_begin(
            replay_directory, error, sizeof(error));
        ok = replaying && stylesheets_load_external_tracked_with_context(
            &document, &stylesheet, &budget, document_url, document_url,
            "unsafe-url", 4, 4096, 4096, 1000, NULL, NULL,
            &resources, &stats);
    }
    lxb_dom_node_t *target = find_element_id(
        document.html == NULL ? NULL
            : lxb_dom_interface_node(document.html),
        "target");
    ComputedStyle computed = style_for_node(&stylesheet, target, NULL);
    ok = ok && target != NULL && stats.loaded == 1
        && stylesheet.count == 3 && computed.color == 0x003300;
    if (replaying) fetch_trace_end();
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    if (replay_written) remove_stylesheet_replay(
        replay_directory, sizeof(replay) / sizeof(replay[0]));
    return ok && clean;
}

static bool test_inline_import_precedes_its_local_rules(void)
{
    static const char document_url[] =
        "https://document.example/article/page.html";
    static const char imported_url[] =
        "https://document.example/article/imported.css";
    static const char html[] =
        "<!doctype html><style>"
        "@import 'imported.css';"
        ".target{color:#003300}"
        "</style><body><div id=target class=target>ordered</div></body>";
    static const StylesheetReplayRecord replay[] = {{
        .url = imported_url,
        .body = ".target{color:#110000}",
        .effective_url = imported_url,
        .request_referer = document_url,
        .request_sec_fetch_site = "same-origin",
        .request_credential_origin = document_url,
        .request_initiator_url = document_url,
        .request_referrer_source = document_url,
        .request_referrer_policy = "unsafe-url",
        .response_cache_control = "max-age=3600",
        .status = 200
    }};
    char replay_directory[128] = {0};
    bool replay_written = write_stylesheet_replay(
        replay_directory, replay, sizeof(replay) / sizeof(replay[0]));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    ExternalStylesheetStats stats = {0};
    bool replaying = false;
    bool ok = replay_written && installed
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    if (ok) {
        char error[256] = {0};
        replaying = fetch_trace_replay_begin(
            replay_directory, error, sizeof(error));
        ok = replaying && stylesheets_load_external_tracked_with_context(
            &document, &stylesheet, &budget, document_url, document_url,
            "unsafe-url", 4, 4096, 4096, 1000, NULL, NULL,
            &resources, &stats);
    }
    lxb_dom_node_t *target = find_element_id(
        document.html == NULL ? NULL
            : lxb_dom_interface_node(document.html),
        "target");
    ComputedStyle computed = style_for_node(&stylesheet, target, NULL);
    ok = ok && target != NULL && stats.discovered == 1
        && stats.attempted == 1 && stats.loaded == 1
        && stylesheet.count == 2 && computed.color == 0x003300;
    if (replaying) fetch_trace_end();
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    if (replay_written) remove_stylesheet_replay(
        replay_directory, sizeof(replay) / sizeof(replay[0]));
    return ok && clean;
}

static bool test_redirected_stylesheet_base_survives_retention(void)
{
    static const char html[] =
        "<!doctype html><link rel=stylesheet "
        "href='https://fixture.test/start.css'>"
        "<body class=target>redirect</body>";
    static const char document_url[] = "https://document.example/page";
    static const char final_url[] =
        "https://cdn.example/styles/final/site.css";
    static const char nested_url[] =
        "https://cdn.example/styles/final/nested.css";
    static const char root_css[] =
        "@import 'nested.css';"
        "@font-face{font-family:Redirected;"
        "src:url(../fonts/face.woff) format(woff)}"
        ".target{font-family:Redirected,serif}";
    static const StylesheetReplayRecord replay[] = {
        {
            .url = "https://fixture.test/start.css",
            .body = root_css,
            .effective_url = final_url,
            .request_referer = document_url,
            .request_sec_fetch_site = "cross-site",
            .request_credential_origin = document_url,
            .request_initiator_url = document_url,
            .request_referrer_source = document_url,
            .request_referrer_policy = "unsafe-url",
            .response_referrer_policy = "no-referrer",
            .response_cache_control = "max-age=3600",
            .status = 200
        },
        {
            .url = nested_url,
            .body = ".target{color:#123456}",
            .effective_url = nested_url,
            .request_sec_fetch_site = "cross-site",
            .request_credential_origin = document_url,
            .request_initiator_url = document_url,
            .request_referrer_source = final_url,
            .request_referrer_policy = "no-referrer",
            .response_cache_control = "max-age=3600",
            .status = 200
        }
    };
    char replay_directory[128] = {0};
    bool replay_written = write_stylesheet_replay(
        replay_directory, replay, sizeof(replay) / sizeof(replay[0]));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession session = {0};
    PocDocument document = {0};
    Stylesheet first = {0}, rebuilt = {0};
    StylesheetDocumentResources resources = {0};
    ExternalStylesheetStats stats = {0};
    bool replaying = false;
    bool ok = replay_written && installed
        && browser_session_init(&session, &budget, 512u * 1024u)
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&first, &budget, &document, 480);
    if (ok) {
        char error[256] = {0};
        replaying = fetch_trace_replay_begin(
            replay_directory, error, sizeof(error));
        ok = replaying && stylesheets_load_external_tracked_with_context(
            &document, &first, &budget, document_url, document_url,
            "unsafe-url", 4, 4096, 4096, 1000, NULL, &session,
            &resources, &stats);
    }
    const StylesheetDocumentResource *root_resource = NULL;
    const StylesheetDocumentResource *nested_resource = NULL;
    for (size_t i = 0; i < resources.count; i++) {
        if (resources.items[i].url != NULL
            && strcmp(resources.items[i].url,
                      "https://fixture.test/start.css") == 0) {
            root_resource = &resources.items[i];
        }
        if (resources.items[i].url != NULL
            && strcmp(resources.items[i].url, nested_url) == 0) {
            nested_resource = &resources.items[i];
        }
    }
    StylesheetWebFontSource source = {0};
    TilefinchRequestContext root_cache_context = {
        .target_url = "https://fixture.test/start.css",
        .initiator_url = document_url,
        .top_level_url = document_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_STYLE
    };
    const BrowserCacheEntry *cached = NULL;
    BrowserCacheStatus root_cache_status = browser_session_cache_match_resource(
        &session, root_cache_context.target_url, &root_cache_context,
        tilefinch_platform_monotonic_time_ns(), &cached);
    ok = ok && resources.count == 2 && root_resource != NULL
        && root_resource->response_provenance_known
        && root_resource->response_url != NULL
        && strcmp(root_resource->response_url, final_url) == 0
        && strcmp(root_resource->response_referrer_policy,
                  "no-referrer") == 0
        && nested_resource != NULL
        && nested_resource->response_provenance_known
        && root_cache_status == BROWSER_CACHE_FRESH
        && cached != NULL && cached->response_url_known
        && cached->response_referrer_policy_known
        && strcmp(browser_cache_entry_response_url(cached), final_url) == 0
        && strcmp(cached->response_referrer_policy, "no-referrer") == 0
        && stylesheet_web_font_source_count(&first) == 1
        && stylesheet_web_font_source(&first, 0, &source)
        && source.source_base_url != NULL
        && strcmp(source.source_base_url, final_url) == 0;
    if (replaying) fetch_trace_end();
    stylesheet_destroy(&first);
    memset(&stats, 0, sizeof(stats));
    if (ok) {
        ok = stylesheet_build(&rebuilt, &budget, &document, 480)
            && stylesheets_load_external_tracked_with_context(
                &document, &rebuilt, &budget, document_url, document_url,
                "unsafe-url", 4, 4096, 4096, 1000, NULL, &session,
                &resources, &stats)
            && stats.retained_body_hits == 2
            && stats.imports_loaded == 1
            && stylesheet_web_font_source_count(&rebuilt) == 1
            && stylesheet_web_font_source(&rebuilt, 0, &source)
            && source.source_base_url != NULL
            && strcmp(source.source_base_url, final_url) == 0;
    }
    stylesheet_destroy(&rebuilt);
    stylesheet_document_resources_destroy(&resources);
    document_destroy(&document);
    browser_session_destroy(&session);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    if (replay_written) remove_stylesheet_replay(
        replay_directory, sizeof(replay) / sizeof(replay[0]));
    return ok && clean;
}

static bool test_absent_response_policy_uses_default_for_import(void)
{
    static const char html[] =
        "<!doctype html><link rel=stylesheet "
        "href='https://sheet.example/root.css'><body>imports</body>";
    static const char document_url[] = "https://document.example/page";
    static const char root_url[] = "https://sheet.example/root.css";
    static const char child_url[] = "https://other.example/child.css";
    static const StylesheetReplayRecord replay[] = {
        {
            .url = root_url,
            .body = "@import 'https://other.example/child.css';",
            .effective_url = root_url,
            .request_sec_fetch_site = "cross-site",
            .request_credential_origin = document_url,
            .request_initiator_url = document_url,
            .request_referrer_source = document_url,
            .request_referrer_policy = "no-referrer",
            .response_cache_control = "max-age=3600",
            .status = 200
        },
        {
            .url = child_url,
            .body = "body{background:#abcdef}",
            .effective_url = child_url,
            .request_referer = "https://sheet.example/",
            .request_sec_fetch_site = "cross-site",
            .request_credential_origin = document_url,
            .request_initiator_url = document_url,
            .request_referrer_source = root_url,
            .request_referrer_policy = "",
            .response_cache_control = "max-age=3600",
            .status = 200
        }
    };
    char replay_directory[128] = {0};
    bool replay_written = write_stylesheet_replay(
        replay_directory, replay, sizeof(replay) / sizeof(replay[0]));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession session = {0};
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    ExternalStylesheetStats stats = {0};
    bool replaying = false;
    bool ok = replay_written && installed
        && browser_session_init(&session, &budget, 512u * 1024u)
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    if (ok) {
        char error[256] = {0};
        replaying = fetch_trace_replay_begin(
            replay_directory, error, sizeof(error));
        ok = replaying && stylesheets_load_external_tracked_with_context(
            &document, &stylesheet, &budget, document_url, document_url,
            "no-referrer", 4, 4096, 4096, 1000, NULL, &session,
            &resources, &stats);
    }
    const StylesheetDocumentResource *root = find_stylesheet_resource(
        &resources, root_url);
    ok = ok && stats.attempted == 2 && stats.loaded == 2
        && stats.imports_discovered == 1 && stats.imports_loaded == 1
        && root != NULL && root->response_provenance_known
        && root->response_referrer_policy[0] == '\0';
    if (replaying) fetch_trace_end();
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    browser_session_destroy(&session);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    if (replay_written) remove_stylesheet_replay(
        replay_directory, sizeof(replay) / sizeof(replay[0]));
    return ok && clean;
}

static bool test_stale_304_merges_response_policy(void)
{
    static const char html[] =
        "<!doctype html><link rel=stylesheet "
        "href='https://document.example/site.css'><body>304</body>";
    static const char document_url[] = "https://document.example/page";
    static const char stylesheet_url[] =
        "https://document.example/site.css";
    static const unsigned char cached_css[] = "body{color:#102030}";
    static const StylesheetReplayRecord replay[] = {
        {
            .url = stylesheet_url,
            .body = "",
            .effective_url = stylesheet_url,
            .request_referer = document_url,
            .request_sec_fetch_site = "same-origin",
            .request_credential_origin = document_url,
            .request_initiator_url = document_url,
            .request_referrer_source = document_url,
            .request_referrer_policy = "unsafe-url",
            .request_if_none_match = "\"v1\"",
            .response_cache_control = "max-age=0",
            .status = 304
        },
        {
            .url = stylesheet_url,
            .body = "",
            .effective_url = stylesheet_url,
            .request_referer = document_url,
            .request_sec_fetch_site = "same-origin",
            .request_credential_origin = document_url,
            .request_initiator_url = document_url,
            .request_referrer_source = document_url,
            .request_referrer_policy = "unsafe-url",
            .request_if_none_match = "\"v1\"",
            .response_referrer_policy = "no-referrer",
            .response_cache_control = "max-age=0",
            .status = 304
        }
    };
    char replay_directory[128] = {0};
    bool replay_written = write_stylesheet_replay(
        replay_directory, replay, sizeof(replay) / sizeof(replay[0]));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession session = {0};
    PocDocument document = {0};
    Stylesheet first = {0}, second = {0};
    ExternalStylesheetStats stats = {0};
    bool replaying = false;
    unsigned char *cached_copy = budget_malloc(
        &budget, sizeof(cached_css) - 1u);
    if (cached_copy != NULL) {
        memcpy(cached_copy, cached_css, sizeof(cached_css) - 1u);
    }
    BrowserSharedBody *cached_body = browser_shared_body_take(
        &budget, cached_copy, sizeof(cached_css) - 1u);
    if (cached_body == NULL) budget_free(&budget, cached_copy);
    TilefinchRequestContext cached_context = {
        .target_url = stylesheet_url,
        .initiator_url = document_url,
        .top_level_url = document_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_STYLE
    };
    TilefinchResourceGrant cached_grant = {
        .destination = TILEFINCH_DESTINATION_STYLE,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .final_same_origin = true,
        .final_same_site = true,
        .cors_validated = true
    };
    bool ok = replay_written && installed
        && browser_session_init(&session, &budget, 512u * 1024u)
        && cached_body != NULL
        && browser_session_cache_put_http_shared_resource(
            &session, stylesheet_url, cached_body, "\"v1\"", NULL,
            "text/css", "max-age=0", NULL, 1, &cached_context,
            &cached_grant)
        && browser_session_cache_set_resource_response_provenance(
            &session, stylesheet_url, &cached_context, stylesheet_url,
            "origin")
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&first, &budget, &document, 480);
    browser_shared_body_release(cached_body);
    if (ok) {
        char error[256] = {0};
        replaying = fetch_trace_replay_begin(
            replay_directory, error, sizeof(error));
        ok = replaying && stylesheets_load_external_with_context(
            &document, &first, &budget, document_url, document_url,
            "unsafe-url", 4, 4096, 4096, 1000, NULL, &session, &stats);
    }
    const BrowserCacheEntry *cached = NULL;
    BrowserCacheStatus cached_status = browser_session_cache_match_resource(
        &session, stylesheet_url, &cached_context,
        tilefinch_platform_monotonic_time_ns(), &cached);
    ok = ok && stats.cache_hits == 1 && cached_status == BROWSER_CACHE_STALE
        && cached != NULL
        && cached->response_url_known
        && cached->response_referrer_policy_known
        && strcmp(cached->response_referrer_policy, "origin") == 0;
    stylesheet_destroy(&first);
    memset(&stats, 0, sizeof(stats));
    if (ok) {
        ok = stylesheet_build(&second, &budget, &document, 480)
            && stylesheets_load_external_with_context(
                &document, &second, &budget, document_url, document_url,
                "unsafe-url", 4, 4096, 4096, 1000, NULL, &session,
                &stats);
    }
    cached_status = browser_session_cache_match_resource(
        &session, stylesheet_url, &cached_context,
        tilefinch_platform_monotonic_time_ns(), &cached);
    ok = ok && stats.cache_hits == 1 && cached_status == BROWSER_CACHE_STALE
        && cached != NULL
        && cached->response_url_known
        && cached->response_referrer_policy_known
        && strcmp(cached->response_referrer_policy, "no-referrer") == 0;
    if (replaying) fetch_trace_end();
    stylesheet_destroy(&second);
    document_destroy(&document);
    browser_session_destroy(&session);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    if (replay_written) remove_stylesheet_replay(
        replay_directory, sizeof(replay) / sizeof(replay[0]));
    return ok && clean;
}

static bool test_unknown_cache_provenance_is_bypassed(void)
{
    static const char html[] =
        "<!doctype html><link rel=stylesheet "
        "href='https://document.example/site.css'><body>cache</body>";
    static const char document_url[] = "https://document.example/page";
    static const char stylesheet_url[] =
        "https://document.example/site.css";
    static const unsigned char stale_css[] = "body{color:#ff0000}";
    static const char network_css[] = "body{color:#00ff00}";
    static const StylesheetReplayRecord replay[] = {{
        .url = stylesheet_url,
        .body = network_css,
        .effective_url = stylesheet_url,
        .request_referer = document_url,
        .request_sec_fetch_site = "same-origin",
        .request_credential_origin = document_url,
        .request_initiator_url = document_url,
        .request_referrer_source = document_url,
        .request_referrer_policy = "unsafe-url",
        .response_cache_control = "max-age=3600",
        .status = 200
    }};
    char replay_directory[128] = {0};
    bool replay_written = write_stylesheet_replay(
        replay_directory, replay, sizeof(replay) / sizeof(replay[0]));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession session = {0};
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ExternalStylesheetStats stats = {0};
    bool replaying = false;
    bool ok = replay_written && installed
        && browser_session_init(&session, &budget, 512u * 1024u)
        && browser_session_cache_put_http(
            &session, stylesheet_url, stale_css, sizeof(stale_css) - 1u,
            NULL, NULL, "text/css", "max-age=3600, immutable", NULL, 1)
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    const BrowserCacheEntry *cached = browser_session_cache_lookup(
        &session, stylesheet_url);
    ok = ok && cached != NULL && !cached->response_url_known
        && !cached->response_referrer_policy_known;
    if (ok) {
        char error[256] = {0};
        replaying = fetch_trace_replay_begin(
            replay_directory, error, sizeof(error));
        ok = replaying && stylesheets_load_external_with_context(
            &document, &stylesheet, &budget, document_url, document_url,
            "unsafe-url", 4, 4096, 4096, 1000, NULL, &session, &stats);
    }
    cached = browser_session_cache_lookup(&session, stylesheet_url);
    TilefinchRequestContext resource_context = {
        .target_url = stylesheet_url,
        .initiator_url = document_url,
        .top_level_url = document_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_STYLE
    };
    const BrowserCacheEntry *authorized = NULL;
    BrowserCacheStatus authorized_status =
        browser_session_cache_match_resource(
            &session, stylesheet_url, &resource_context,
            tilefinch_platform_monotonic_time_ns(), &authorized);
    ok = ok && stats.attempted == 1 && stats.loaded == 1
        && stats.cache_hits == 0 && cached != NULL
        && !cached->response_url_known
        && cached->length == sizeof(stale_css) - 1u
        && authorized_status == BROWSER_CACHE_FRESH
        && authorized != NULL && authorized->response_url_known
        && authorized->response_referrer_policy_known
        && authorized->length == sizeof(network_css) - 1u
        && memcmp(authorized->data, network_css,
                  sizeof(network_css) - 1u) == 0;
    if (replaying) fetch_trace_end();
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    browser_session_destroy(&session);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    if (replay_written) remove_stylesheet_replay(
        replay_directory, sizeof(replay) / sizeof(replay[0]));
    return ok && clean;
}

static bool test_css_images_keep_declaring_source_context(void)
{
    static const char html[] =
        "<!doctype html><base href='https://assets.document.example/base/'>"
        "<style>#inline-image{background-image:url(shared.svg)}</style>"
        "<link rel=stylesheet "
        "href='https://document.example/theme/start.css'>"
        "<body><div id=inline-image></div>"
        "<img id=html-image src=shared.svg>"
        "<div class=external-image></div></body>";
    static const char external_css[] =
        ".external-image{background-image:url(shared.svg);"
        "mask-image:url(mask.svg)}";
    static const char svg[] =
        "<svg xmlns='http://www.w3.org/2000/svg' width='1' height='1'>"
        "<rect width='1' height='1' fill='#123456'/></svg>";
    static const char document_url[] =
        "https://document.example/articles/page.html";
    static const char document_base_url[] =
        "https://assets.document.example/base/";
    static const char stylesheet_request_url[] =
        "https://document.example/theme/start.css";
    static const char stylesheet_response_url[] =
        "https://cdn.example/theme/v2/main.css";
    static const char document_image_url[] =
        "https://assets.document.example/base/shared.svg";
    static const char external_image_url[] =
        "https://cdn.example/theme/v2/shared.svg";
    static const char external_mask_url[] =
        "https://cdn.example/theme/v2/mask.svg";
    static const char image_accept[] =
#if defined(TILEFINCH_DISABLE_GIF)
        "image/png,image/jpeg,image/webp,image/svg+xml,image/*;q=0.8,"
        "*/*;q=0.5";
#else
        "image/png,image/jpeg,image/gif,image/webp,image/svg+xml,"
        "image/*;q=0.8,*/*;q=0.5";
#endif
    static const StylesheetReplayRecord replay[] = {
        {
            .url = stylesheet_request_url,
            .body = external_css,
            .effective_url = stylesheet_response_url,
            .request_referer = document_url,
            .request_sec_fetch_site = "same-origin",
            .request_credential_origin = document_url,
            .request_initiator_url = document_url,
            .request_referrer_source = document_url,
            .request_referrer_policy = "unsafe-url",
            .response_referrer_policy = "no-referrer",
            .response_cache_control = "max-age=3600",
            .status = 200
        },
        {
            .url = document_image_url,
            .body = svg,
            .effective_url = document_image_url,
            .content_type = "image/svg+xml",
            .request_accept = image_accept,
            .request_sec_fetch_dest = "image",
            .request_sec_fetch_mode = "no-cors",
            .request_referer = document_url,
            .request_sec_fetch_site = "same-site",
            .request_credential_origin = document_url,
            .request_initiator_url = document_url,
            .request_referrer_source = document_url,
            .request_referrer_policy = "unsafe-url",
            .response_cache_control = "max-age=3600",
            .status = 200,
            .request_send_low_client_hints = true
        },
        {
            .url = external_image_url,
            .body = svg,
            .effective_url = external_image_url,
            .content_type = "image/svg+xml",
            .request_accept = image_accept,
            .request_sec_fetch_dest = "image",
            .request_sec_fetch_mode = "no-cors",
            .request_sec_fetch_site = "cross-site",
            .request_credential_origin = document_url,
            .request_initiator_url = document_url,
            .request_referrer_source = stylesheet_response_url,
            .request_referrer_policy = "no-referrer",
            .response_cache_control = "max-age=3600",
            .request_cookie_bytes = 3,
            .status = 200,
            .request_send_low_client_hints = true
        },
        {
            .url = external_mask_url,
            .body = svg,
            .effective_url = external_mask_url,
            .content_type = "image/svg+xml",
            .request_accept = image_accept,
            .request_sec_fetch_dest = "image",
            .request_sec_fetch_mode = "no-cors",
            .request_sec_fetch_site = "cross-site",
            .request_credential_origin = document_url,
            .request_initiator_url = document_url,
            .request_referrer_source = stylesheet_response_url,
            .request_referrer_policy = "no-referrer",
            .response_cache_control = "max-age=3600",
            .request_cookie_bytes = 3,
            .status = 200,
            .request_send_low_client_hints = true
        }
    };
    char replay_directory[128] = {0};
    bool replay_written = write_stylesheet_replay(
        replay_directory, replay, sizeof(replay) / sizeof(replay[0]));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession session = {0};
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    ImageResources images = {0};
    ExternalStylesheetStats stylesheet_stats = {0};
    FetchTraceReplayStats replay_stats = {0};
    bool replaying = false;
    char cookie_header[64] = {0};
    char replay_error[256] = {0};
    TilefinchRequestContext cookie_context = {
        .target_url = external_image_url,
        .initiator_url = document_url,
        .top_level_url = document_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_IMAGE
    };
    bool ok = replay_written && installed
        && browser_session_init(&session, &budget, 512u * 1024u)
        && browser_session_cookie_set_http_context(
            &session, &cookie_context,
            "n=1; Path=/; Secure; SameSite=None; Partitioned")
        && browser_session_cookie_set_http(
            &session, "https://cdn.example/",
            "l=1; Path=/; Secure; SameSite=Lax")
        && browser_session_cookie_header_context(
            &session, &cookie_context, cookie_header,
            sizeof(cookie_header))
        && strcmp(cookie_header, "n=1") == 0
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    if (ok) {
        replaying = fetch_trace_replay_begin(
            replay_directory, replay_error, sizeof(replay_error));
        ok = replaying && stylesheets_load_external_tracked_with_context(
            &document, &stylesheet, &budget, document_base_url,
            document_url, "unsafe-url", 4, 4096, 4096, 1000, NULL,
            &session, &resources, &stylesheet_stats)
            && images_load_external(
                &document, &stylesheet, &images, &budget,
                document_base_url, document_url, "unsafe-url", 8,
                16u * 1024u, 4096, 4096, 1000, NULL, &session)
            && fetch_trace_replay_stats(&replay_stats);
    }
    ok = ok && stylesheet_stats.attempted == 1
        && stylesheet_stats.loaded == 1
        /* The HTML image and inline declaration intentionally share their
           document URL; the text-identical external declaration must not be
           collapsed into that context. */
        && images.stats.discovered == 4 && images.stats.attempted == 3
        && images.stats.loaded == 3 && images.stats.duplicate == 1
        && images.stats.failed == 0 && images.count == 4
        && replay_stats.record_count == 4
        && replay_stats.request_count == 4
        && replay_stats.matched_request_count == 4
        && replay_stats.served_request_count == 4
        && replay_stats.unmatched_request_count == 0
        && replay_stats.request_shape_mismatch_count == 0;
    if (!ok) {
        fprintf(stderr,
                "css image provenance cookie='%s' replay='%s' "
                "css=%zu/%zu image=%zu/%zu/%zu/%zu failed=%zu "
                "replay=%zu/%zu/%zu/%zu mismatch=%zu\n",
                cookie_header, replay_error,
                stylesheet_stats.loaded, stylesheet_stats.attempted,
                images.count, images.stats.discovered,
                images.stats.loaded, images.stats.attempted,
                images.stats.failed, replay_stats.request_count,
                replay_stats.matched_request_count,
                replay_stats.served_request_count,
                replay_stats.unmatched_request_count,
                replay_stats.request_shape_mismatch_count);
    }
    if (replaying) fetch_trace_end();
    images_destroy(&images);
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    browser_session_destroy(&session);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    if (replay_written) remove_stylesheet_replay(
        replay_directory, sizeof(replay) / sizeof(replay[0]));
    return ok && clean;
}

static bool test_css_data_svg_mask_uses_bounded_image_pipeline(void)
{
    static const char html[] =
        "<!doctype html><style>"
        "#icon{display:block;width:20px;height:20px;background:#54595d;"
        "mask-image:url(\"data:image/svg+xml;utf8,"
        "<svg xmlns=\\\"http://www.w3.org/2000/svg\\\" width=\\\"20\\\" "
        "height=\\\"20\\\" viewBox=\\\"0 0 20 20\\\" fill=\\\"%23000\\\">"
        "<path d=\\\"m4.66 7.93 5.34 5.33 5.34-5.33L14.26 6.87 "
        "10 11.1 5.74 6.87z\\\"/></svg>\")}"
        "</style><body><span id=icon></span></body>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    bool ok = installed
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480)
        && images_load_external(
            &document, &stylesheet, &images, &budget,
            "https://fixture.test/page", "https://fixture.test/page",
            "strict-origin-when-cross-origin", 4, 4096, 2048, 4096,
            1000, NULL, NULL);
    lxb_dom_node_t *icon = ok ? find_element_id(
        lxb_dom_interface_node(document.html), "icon") : NULL;
    const ImageResource *mask = icon == NULL
        ? NULL : images_find_mask_node(&images, icon);
    ok = ok && images.stats.discovered == 1
        && images.stats.attempted == 1 && images.stats.loaded == 1
        && images.stats.masks_loaded == 1 && images.stats.failed == 0
        && images.stats.unsupported == 0 && images.count == 1
        && mask != NULL && mask->pixels != NULL
        && mask->width == 20 && mask->height == 20;
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_webp_url_uses_same_origin_jpeg_sibling(void)
{
    static const char html[] =
        "<!doctype html><body><img id=hero "
        "src='image?id=hero.webp&quality=80'>";
    static const char document_url[] = "https://image.example/page";
    static const char jpeg_url[] =
        "https://image.example/image?id=hero.jpg&quality=80";
    static const char svg[] =
        "<svg xmlns='http://www.w3.org/2000/svg' width='2' height='1'>"
        "<rect width='2' height='1' fill='#123456'/></svg>";
    static const char image_accept[] =
#if defined(TILEFINCH_DISABLE_GIF)
        "image/png,image/jpeg,image/webp,image/svg+xml,image/*;q=0.8,"
        "*/*;q=0.5";
#else
        "image/png,image/jpeg,image/gif,image/webp,image/svg+xml,"
        "image/*;q=0.8,*/*;q=0.5";
#endif
    static const StylesheetReplayRecord replay[] = {{
        .url = jpeg_url,
        .body = svg,
        .effective_url = jpeg_url,
        .content_type = "image/svg+xml",
        .request_accept = image_accept,
        .request_sec_fetch_dest = "image",
        .request_sec_fetch_mode = "no-cors",
        .request_referer = document_url,
        .request_sec_fetch_site = "same-origin",
        .request_credential_origin = document_url,
        .request_initiator_url = document_url,
        .request_referrer_source = document_url,
        .request_referrer_policy = "strict-origin-when-cross-origin",
        .status = 200,
        .request_send_low_client_hints = true
    }};
    char replay_directory[128] = {0};
    bool replay_written = write_stylesheet_replay(
        replay_directory, replay, sizeof(replay) / sizeof(replay[0]));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    FetchTraceReplayStats replay_stats = {0};
    char replay_error[256] = {0};
    bool replaying = false;
    bool ok = replay_written && installed
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    if (ok) {
        replaying = fetch_trace_replay_begin(
            replay_directory, replay_error, sizeof(replay_error));
        ok = replaying && images_load_external(
            &document, &stylesheet, &images, &budget,
            document_url, document_url, "strict-origin-when-cross-origin",
            2, 4096, 2048, 4096, 1000, NULL, NULL)
            && fetch_trace_replay_stats(&replay_stats);
    }
    ok = ok && images.stats.compatible_format_rewrites == 1
        && images.stats.loaded == 1 && images.stats.unsupported == 0
        && replay_stats.matched_request_count == 1
        && replay_stats.unmatched_request_count == 0;
    if (!ok) {
        fprintf(stderr,
                "webp sibling replay='%s' rewrites=%zu loaded=%zu "
                "unsupported=%zu requests=%zu/%zu\n",
                replay_error, images.stats.compatible_format_rewrites,
                images.stats.loaded, images.stats.unsupported,
                replay_stats.matched_request_count,
                replay_stats.unmatched_request_count);
    }
    if (replaying) fetch_trace_end();
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    if (replay_written) remove_stylesheet_replay(
        replay_directory, sizeof(replay) / sizeof(replay[0]));
    return ok && clean;
}

static bool test_css_paint_layers_keep_distinct_resources(void)
{
    static const char html[] =
        "<!doctype html><style>#layers{display:block;width:20px;height:20px;"
        "background-image:"
        "url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='1' height='1'%3E%3Crect width='1' height='1' fill='%23112233'/%3E%3C/svg%3E\"),"
        "url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='2' height='1'%3E%3Crect width='2' height='1' fill='%23445566'/%3E%3C/svg%3E\");"
        "mask-image:"
        "url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='3' height='1'%3E%3Crect width='3' height='1'/%3E%3C/svg%3E\"),"
        "url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='4' height='1'%3E%3Crect width='4' height='1'/%3E%3C/svg%3E\")"
        "}</style><body><div id=layers></div></body>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ImageResources images = {0};
    bool ok = installed
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480)
        && images_load_external(
            &document, &stylesheet, &images, &budget,
            "https://fixture.test/page", "https://fixture.test/page",
            "strict-origin-when-cross-origin", 8, 16384, 4096, 16384,
            1000, NULL, NULL);
    lxb_dom_node_t *node = ok ? find_element_id(
        lxb_dom_interface_node(document.html), "layers") : NULL;
    ComputedStyle style = node == NULL ? (ComputedStyle) {0}
        : style_for_node(&stylesheet, node, NULL);
    const StylePaintStack *paint = stylesheet_paint_stack(
        &stylesheet, computed_style_paint_stack_id(&style));
    ok = ok && paint != NULL && paint->background_count == 2
        && paint->mask_count == 2 && images.count == 4
        && images.stats.backgrounds_loaded == 2
        && images.stats.masks_loaded == 2;
    if (ok) {
        const ImageResource *bg0 = images_find_background_source(
            &images, node, paint->backgrounds[0].image, PSEUDO_NONE);
        const ImageResource *bg1 = images_find_background_source(
            &images, node, paint->backgrounds[1].image, PSEUDO_NONE);
        const ImageResource *mask0 = images_find_mask_source(
            &images, node, paint->masks[0].image, PSEUDO_NONE);
        const ImageResource *mask1 = images_find_mask_source(
            &images, node, paint->masks[1].image, PSEUDO_NONE);
        ok = bg0 != NULL && bg1 != NULL && mask0 != NULL && mask1 != NULL
            && bg0 != bg1 && mask0 != mask1
            && bg0->source_width == 1 && bg1->source_width == 2
            && mask0->source_width == 3 && mask1->source_width == 4;
    }
    if (!ok) {
        fprintf(stderr,
                "paint layers paint=%p bg=%u mask=%u count=%zu "
                "loaded=%zu backgrounds=%zu masks=%zu failed=%zu "
                "unsupported=%zu\n",
                (void *) paint,
                paint == NULL ? 0u : (unsigned) paint->background_count,
                paint == NULL ? 0u : (unsigned) paint->mask_count,
                images.count, images.stats.loaded,
                images.stats.backgrounds_loaded, images.stats.masks_loaded,
                images.stats.failed, images.stats.unsupported);
    }
    images_destroy(&images);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_http_error_is_terminal(void)
{
    static const char html[] =
        "<!doctype html><link rel=stylesheet "
        "href='https://fixture.test/missing.css'><body>missing</body>";
    char replay_directory[256] = {0};
    bool replay_written = write_http_error_replay(
        replay_directory, sizeof(replay_directory));
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    ExternalStylesheetStats stats = {0};
    bool replaying = false;
    bool ok = replay_written && installed
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    size_t rules_before = stylesheet.count;
    if (ok) {
        char error[256] = {0};
        replaying = fetch_trace_replay_begin(
            replay_directory, error, sizeof(error));
        ok = replaying && stylesheets_load_external_tracked(
            &document, &stylesheet, &budget, "https://fixture.test/page",
            4, 4096, 4096, 1000, NULL, NULL, &resources, &stats);
    }
    ok = ok && stats.attempted == 1 && stats.loaded == 0
        && stats.failed == 1 && stats.terminal_failures == 1
        && stylesheet.count == rules_before && resources.count == 1
        && resources.items[0].state
               == STYLESHEET_DOCUMENT_RESOURCE_TERMINAL_FAILURE
        && stylesheet_document_resources_link_state(
               &resources, "https://fixture.test/page",
               "https://fixture.test/missing.css",
               strlen("https://fixture.test/missing.css"))
               == STYLESHEET_DOCUMENT_RESOURCE_TERMINAL_FAILURE;
    if (replaying) fetch_trace_end();
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    if (replay_written) remove_http_error_replay(replay_directory);
    return ok && clean;
}

static bool test_invalid_href_has_terminal_state(void)
{
    static const char html[] =
        "<!doctype html><link rel=stylesheet "
        "href='javascript:alert(1)'><body>invalid</body>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    ExternalStylesheetStats stats = {0};
    bool ok = installed
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480)
        && stylesheets_load_external_tracked(
            &document, &stylesheet, &budget, "https://fixture.test/page",
            4, 4096, 4096, 1000, NULL, NULL, &resources, &stats)
        && stats.discovered == 1 && stats.attempted == 0
        && stats.failed == 1 && resources.count == 0
        && stylesheet_document_resources_link_state(
               &resources, "https://fixture.test/page",
               "javascript:alert(1)", strlen("javascript:alert(1)"))
               == STYLESHEET_DOCUMENT_RESOURCE_TERMINAL_FAILURE
        && stylesheet_document_resources_link_state(
               &resources, "https://fixture.test/page", NULL, 0)
               == STYLESHEET_DOCUMENT_RESOURCE_EMPTY;
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_blocker_bypasses_fresh_cached_stylesheet(void)
{
    static const char html[] =
        "<!doctype html><link rel=stylesheet "
        "href='https://doubleclick.net/ad.css'><body>blocked</body>";
    static const char document_url[] = "https://publisher.example/page";
    static const char stylesheet_url[] = "https://doubleclick.net/ad.css";
    static const unsigned char cached_css[] = "body{display:none}";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession session = {0};
    ContentBlocker *blocker = NULL;
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ExternalStylesheetStats stats = {0};
    bool ok = installed
        && browser_session_init(&session, &budget, 512u * 1024u)
        && (blocker = content_blocker_create(&budget)) != NULL
        && content_blocker_configure(
               blocker, CONTENT_BLOCKER_BASIC, NULL);
    if (ok) session.content_blocker = blocker;
    ok = ok && browser_session_cache_put_http(
            &session, stylesheet_url, cached_css, sizeof(cached_css) - 1u,
            NULL, NULL, "text/css", "max-age=3600", NULL,
            tilefinch_platform_monotonic_time_ns())
        && browser_session_cache_set_response_provenance(
            &session, stylesheet_url, stylesheet_url, "")
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480)
        && stylesheets_load_external_with_context(
            &document, &stylesheet, &budget, document_url, document_url,
            "", 4, 4096, 4096, 50, NULL, &session, &stats)
        && stats.cache_hits == 0 && stats.loaded == 0 && stats.failed == 1;
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    session.content_blocker = NULL;
    content_blocker_destroy(blocker);
    browser_session_destroy(&session);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

/* A CORS-authorized response is scoped to the requesting origin and
   credentials mode. It must not become a generic URL-only cache entry. */
static bool test_cors_stylesheet_uses_partitioned_resource_cache(void)
{
    static const char document_url[] = "https://publisher.example/page";
    static const char document_origin[] = "https://publisher.example";
    static const char stylesheet_url[] = "https://cdn.example/private.css";
    static const char css[] = "body{color:#123456}";
    static const char html[] =
        "<!doctype html><link rel=stylesheet crossorigin=use-credentials "
        "href='https://cdn.example/private.css'><body>private</body>";
    char directory[128] = "/tmp/tilefinch-cors-cache-XXXXXX";
    bool fixture = mkdtemp(directory) != NULL;
    char body_path[192] = {0}, meta_path[192] = {0}, clock_path[192] = {0};
    if (fixture) {
        fixture = snprintf(body_path, sizeof(body_path), "%s/0000.body",
                           directory) > 0
            && snprintf(meta_path, sizeof(meta_path), "%s/0000.meta",
                        directory) > 0
            && snprintf(clock_path, sizeof(clock_path), "%s/trace.meta",
                        directory) > 0;
    }
    FILE *body = fixture ? fopen(body_path, "wb") : NULL;
    fixture = body != NULL
        && fwrite(css, 1, sizeof(css) - 1u, body) == sizeof(css) - 1u
        && fclose(body) == 0;
    FILE *meta = fixture ? fopen(meta_path, "wb") : NULL;
    fixture = meta != NULL && fprintf(
        meta,
        "psp-http-trace=10\n"
        "cookie-values=redacted\n"
        "method=GET\nurl=%s\nlogical-request-url=%s\nsuccess=1\n"
        "async-delay-pumps=0\nexternal-cancel=0\n"
        "transport-timeout=0\nredirect-origin-tainted=0\nerror=\n"
        "request-body-length=0\n"
        "request-body-hash=cbf29ce484222325\n"
        "request-content-type=\nrequest-cookie-bytes=10\n"
        "request-has-cf-clearance=0\nrequest-extra-header-bytes=0\n"
        "request-extra-header-shape=\nrequest-allow-http-errors=0\n"
        "request-enforce-cors=1\n"
        "request-redirect-same-origin-only=0\n"
        "request-cors-cached-response-validated=0\n"
        "request-if-none-match=\nrequest-if-modified-since=\n"
        "request-referer=%s\nrequest-origin=%s\n"
        "request-accept=text/css,*/*;q=0.1\n"
        "request-sec-fetch-dest=style\nrequest-sec-fetch-mode=cors\n"
        "request-sec-fetch-site=cross-site\n"
        "request-send-client-hints=0\nrequest-client-hint-tokens=\n"
        "request-client-hint-origin=\nrequest-send-low-client-hints=0\n"
        "request-sec-fetch-user=0\nrequest-upgrade-insecure=0\n"
        "request-user-agent=" TILEFINCH_BROWSER_USER_AGENT "\n"
        "request-diagnostic-mobile-safari=0\nrequest-credentials=0\n"
        "request-credential-origin=%s\nrequest-initiator-url=%s\n"
        "request-referrer-source=%s\nrequest-referrer-policy=unsafe-url\n"
        "status=200\nlength=%zu\nresponse-body-hash=%016llx\n"
        "effective-url=%s\ncontent-type=text/css\netag=\n"
        "last-modified=\ncf-mitigated=\naccept-ch=\ncritical-ch=\n"
        "server=fixture\ncf-ray=\n"
        "response-referrer-policy-metadata-valid=1\n"
        "response-referrer-policy-present=0\n"
        "response-referrer-policy=\n"
        "response-security-headers-truncated=0\n"
        "response-header-count=4\n"
        "set-cookie-count=0\nresponse-header-0=content-type: text/css\n"
        "response-header-1=cache-control: max-age=3600\n"
        "response-header-2=access-control-allow-origin: %s\n"
        "response-header-3=access-control-allow-credentials: true\n",
        stylesheet_url, stylesheet_url, document_url, document_origin,
        document_url, document_url, document_url,
        sizeof(css) - 1u,
        (unsigned long long) stylesheet_replay_body_hash(
            css, sizeof(css) - 1u),
        stylesheet_url, document_origin) > 0
        && fclose(meta) == 0;
    FILE *clock = fixture ? fopen(clock_path, "wb") : NULL;
    fixture = clock != NULL
        && fprintf(clock,
                   "psp-http-trace-clock=1\norigin-ms=1000\n"
                   "capture-complete=yes\nrecord-count=1\n") > 0
        && fclose(clock) == 0;

    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession session = {0};
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    ExternalStylesheetStats stats = {0};
    bool replaying = false;
    bool ok = fixture && installed
        && browser_session_init(&session, &budget, 512u * 1024u)
        /* This test exercises the resource-cache partition, not the default
           third-party-cookie block. Opt into its compatibility exception so
           the credentialed replay keeps the pre-existing request shape. */
        && browser_session_set_third_party_cookie_site_allowed(
               &session, document_url, true)
        && browser_session_cookie_set_http(
               &session, stylesheet_url,
               "sid=secret; Secure; SameSite=None")
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    if (ok) {
        char error[256] = {0};
        replaying = fetch_trace_replay_begin(directory, error, sizeof(error));
        ok = replaying && stylesheets_load_external_with_context(
            &document, &stylesheet, &budget, document_url, document_url,
            "unsafe-url", 4, 4096, 4096, 1000, NULL, &session, &stats);
    }
    TilefinchRequestContext resource_context = {
        .target_url = stylesheet_url,
        .initiator_url = document_url,
        .top_level_url = document_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_STYLE
    };
    const BrowserCacheEntry *cached = NULL;
    ok = ok && stats.loaded == 1
        && browser_session_cache_lookup(&session, stylesheet_url) == NULL
        && browser_session_cache_match_resource(
               &session, stylesheet_url, &resource_context,
               tilefinch_platform_monotonic_time_ns(), &cached)
               == BROWSER_CACHE_FRESH
        && cached != NULL && cached->resource_grant_valid
        && cached->resource_grant.cors_validated;
    if (replaying) fetch_trace_end();
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    browser_session_destroy(&session);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    (void) unlink(body_path);
    (void) unlink(meta_path);
    (void) unlink(clock_path);
    if (fixture) (void) rmdir(directory);
    return ok && clean;
}

/* The raw preload scanner and the ordered loader must agree on CORS mode.
   Otherwise a successful speculative response is thrown away and fetched a
   second time at the parser's first stylesheet checkpoint. */
static bool test_cors_stylesheet_reuses_document_preload(void)
{
    static const char page[] = "https://publisher.example/page";
    static const char url[] = "https://cdn.example/theme.css";
    static const char css[] = "#probe{display:flex;color:#123456}";
    static const char html[] =
        "<!doctype html><link rel=stylesheet crossorigin=anonymous "
        "href='https://cdn.example/theme.css'>"
        "<body id=probe>preloaded</body>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {.budget = &budget};
    ExternalStylesheetStats stats = {0};
    unsigned char *bytes = budget_malloc(
        &budget, sizeof(css) - 1u);
    if (bytes != NULL) memcpy(bytes, css, sizeof(css) - 1u);
    BrowserSharedBody *body = browser_shared_body_take(
        &budget, bytes, sizeof(css) - 1u);
    if (body == NULL) budget_free(&budget, bytes);
    bool ok = installed && body != NULL
        && stylesheet_document_resources_retain(
               &resources, url, url, "", body, sizeof(css) - 1u, true,
               TILEFINCH_CREDENTIALS_SAME_ORIGIN)
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480)
        && stylesheets_load_external_tracked_with_context(
               &document, &stylesheet, &budget, page, page, "", 4, 4096,
               4096, 1000, NULL, NULL, &resources, &stats);
    lxb_dom_node_t *probe = ok ? find_element_id(
        lxb_dom_interface_node(document.html), "probe") : NULL;
    ComputedStyle style = probe == NULL ? (ComputedStyle) {0}
        : style_for_node(&stylesheet, probe, NULL);
    ok = ok && stats.attempted == 1 && stats.loaded == 1
        && stats.failed == 0 && stats.cache_hits == 1
        && stats.retained_body_hits == 1 && style.display == DISPLAY_FLEX
        && style.color == 0x123456;
    browser_shared_body_release(body);
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_stylesheet_pressure_omits_optional_sheet(void)
{
    static const char html[] =
        "<!doctype html><link rel=stylesheet "
        "href='https://fixture.test/layout.css'>"
        "<body id=probe class=external-flex>readable fallback</body>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    ExternalStylesheetStats stats = {0};
    bool replaying = false;
    bool ok = installed
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    size_t original_limit = budget.limit;
    if (ok) {
        /* Less than the parse reserve remains, but enough space is present
           for the bounded loader itself and a usable default cascade. */
        budget.limit = budget.current + 2u * MIB;
        replaying = begin_layout_css_replay();
        ok = replaying && stylesheets_load_external_tracked(
            &document, &stylesheet, &budget, "https://fixture.test/page",
            4, 4096, 4096, 1000, NULL, NULL, &resources, &stats);
        budget.limit = original_limit;
    }
    lxb_dom_node_t *probe = ok ? find_element_id(
        lxb_dom_interface_node(document.html), "probe") : NULL;
    ComputedStyle style = probe == NULL ? (ComputedStyle) {0}
        : style_for_node(&stylesheet, probe, NULL);
    ok = ok && stats.skipped_pressure == 1 && stats.failed == 0
        && style.display != DISPLAY_FLEX
        && budget.pressure[BUDGET_PRESSURE_STYLESHEET].decisions >= 1;
    if (!ok) {
        fprintf(stderr,
                "stylesheet pressure fallback skipped=%zu failed=%zu "
                "display=%d decisions=%zu current=%zu limit=%zu\n",
                stats.skipped_pressure, stats.failed, style.display,
                budget.pressure[BUDGET_PRESSURE_STYLESHEET].decisions,
                budget.current, budget.limit);
    }
    if (replaying) fetch_trace_end();
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_pressure_skipped_sheet_preserves_later_byte_quota(void)
{
    static const char page[] = "https://fixture.test/page";
    static const char large_url[] = "https://fixture.test/large.css";
    static const char late_url[] = "https://fixture.test/late.css";
    static const char late_css[] = "#late{display:flex}";
    static const char html[] =
        "<!doctype html>"
        "<link rel=stylesheet href='https://fixture.test/large.css'>"
        "<style></style>"
        "<link rel=stylesheet href='https://fixture.test/late.css'>"
        "<body id=late>late rules remain eligible</body>";
    enum { LARGE_BYTES = 20000 };
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {.budget = &budget};
    ExternalStylesheetStats stats = {0};
    unsigned char *large_bytes = budget_calloc(&budget, LARGE_BYTES, 1u);
    unsigned char *late_bytes = budget_malloc(&budget, sizeof(late_css) - 1u);
    if (late_bytes != NULL) {
        memcpy(late_bytes, late_css, sizeof(late_css) - 1u);
    }
    BrowserSharedBody *large_body = browser_shared_body_take(
        &budget, large_bytes, LARGE_BYTES);
    if (large_body == NULL) budget_free(&budget, large_bytes);
    BrowserSharedBody *late_body = browser_shared_body_take(
        &budget, late_bytes, sizeof(late_css) - 1u);
    if (late_body == NULL) budget_free(&budget, late_bytes);
    bool ok = installed && large_body != NULL && late_body != NULL
        && stylesheet_document_resources_retain(
               &resources, large_url, large_url, "", large_body,
               LARGE_BYTES, false, TILEFINCH_CREDENTIALS_INCLUDE)
        && stylesheet_document_resources_retain(
               &resources, late_url, late_url, "", late_body,
               sizeof(late_css) - 1u, false,
               TILEFINCH_CREDENTIALS_INCLUDE)
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&stylesheet, &budget, &document, 480);
    browser_shared_body_release(large_body);
    browser_shared_body_release(late_body);
    size_t original_limit = budget.limit;
    if (ok) {
        /* The 20 KiB sheet needs a 60 KiB parse allowance and is refused;
           the minimum 32 KiB allowance for the tiny late sheet still fits. */
        budget.limit = budget.current + 3u * MIB + 55u * 1024u;
        ok = stylesheets_load_external_tracked(
            &document, &stylesheet, &budget, page, 4, LARGE_BYTES,
            LARGE_BYTES, 1000, NULL, NULL, &resources, &stats);
        budget.limit = original_limit;
    }
    lxb_dom_node_t *late = ok ? find_element_id(
        lxb_dom_interface_node(document.html), "late") : NULL;
    ComputedStyle style = late == NULL ? (ComputedStyle) {0}
        : style_for_node(&stylesheet, late, NULL);
    ok = ok && stats.skipped_pressure == 1
        && stats.bytes == sizeof(late_css) - 1u
        && style.display == DISPLAY_FLEX;
    if (!ok) {
        fprintf(stderr,
                "stylesheet pressure quota discovered=%zu attempted=%zu "
                "loaded=%zu failed=%zu retained=%zu limit-skip=%zu "
                "pressure-skip=%zu bytes=%zu display=%d current=%zu "
                "limit=%zu\n",
                stats.discovered, stats.attempted, stats.loaded, stats.failed,
                stats.retained_body_hits, stats.skipped_limit,
                stats.skipped_pressure, stats.bytes, style.display,
                budget.current, budget.limit);
    }
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_compiled_fragment_reuses_external_css_with_new_inline_css(void)
{
    static const char document_url[] = "https://fixture.test/second";
    static const char stylesheet_url[] = "https://fixture.test/shared.css";
    static const unsigned char css[] =
        "/* This deliberately compressible prefix keeps the structural "
        "artifact smaller than its source, as required by the RAM cache. */"
        ".shared{display:flex;color:#123456}"
        ".shared>span{padding-left:7px}";
    static const char first_html[] =
        "<!doctype html><link rel=stylesheet href='/shared.css'>"
        "<style>.shared{color:#abcdef}</style>"
        "<div id=target class=shared><span>one</span></div>";
    static const char second_html[] =
        "<!doctype html><link rel=stylesheet href='/shared.css'>"
        "<style>.shared{color:#fedcba}</style>"
        "<div id=target class=shared><span>two</span></div>";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    BrowserSession session = {0};
    PocDocument first_document = {0}, second_document = {0};
    Stylesheet first_sheet = {0}, second_sheet = {0};
    ExternalStylesheetStats first_stats = {0}, second_stats = {0};
    unsigned char *copy = budget_malloc(&budget, sizeof(css) - 1u);
    if (copy != NULL) memcpy(copy, css, sizeof(css) - 1u);
    BrowserSharedBody *body = browser_shared_body_take(
        &budget, copy, sizeof(css) - 1u);
    if (body == NULL) budget_free(&budget, copy);
    TilefinchRequestContext request = {
        .target_url = stylesheet_url,
        .initiator_url = document_url,
        .top_level_url = document_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_STYLE
    };
    TilefinchResourceGrant grant = {
        .destination = TILEFINCH_DESTINATION_STYLE,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .final_same_origin = true,
        .final_same_site = true,
        .cors_validated = true
    };
    bool ok = installed && body != NULL
        && browser_session_init(&session, &budget, 1024u * 1024u)
        && browser_session_cache_put_http_shared_resource(
               &session, stylesheet_url, body, NULL, NULL, "text/css",
               "max-age=3600", NULL,
               tilefinch_platform_monotonic_time_ns(), &request, &grant)
        && browser_session_cache_set_resource_response_provenance(
               &session, stylesheet_url, &request, stylesheet_url, "")
        && document_parse(&first_document, &budget, first_html,
                          sizeof(first_html) - 1u, 17)
        && stylesheet_build(&first_sheet, &budget, &first_document, 480)
        && stylesheets_load_external_with_context(
               &first_document, &first_sheet, &budget, document_url,
               document_url, "", 4, 4096, 4096, 1000, NULL, &session,
               &first_stats);
    browser_shared_body_release(body);
    lxb_dom_node_t *first_target = ok ? find_element_id(
        lxb_dom_interface_node(first_document.html), "target") : NULL;
    ComputedStyle first_style = first_target == NULL
        ? (ComputedStyle) {0} : style_for_node(&first_sheet, first_target, NULL);
    ok = ok && first_style.display == DISPLAY_FLEX
        && first_style.color == 0xabcdef
        && first_stats.compiled_fragment_hits == 0
        && first_stats.compiled_fragment_misses == 1
        && first_stats.compiled_fragment_stores == 1
        && first_stats.parsed_ir_hits == 0
        && first_stats.parsed_ir_misses == 1
        && first_stats.parsed_ir_stores == 1;
    stylesheet_destroy(&first_sheet);
    document_destroy(&first_document);
    if (ok) {
        ok = document_parse(&second_document, &budget, second_html,
                            sizeof(second_html) - 1u, 17)
            && stylesheet_build(
                   &second_sheet, &budget, &second_document, 480)
            && stylesheets_load_external_with_context(
                   &second_document, &second_sheet, &budget, document_url,
                   document_url, "", 4, 4096, 4096, 1000, NULL, &session,
                   &second_stats);
    }
    lxb_dom_node_t *second_target = ok ? find_element_id(
        lxb_dom_interface_node(second_document.html), "target") : NULL;
    ComputedStyle second_style = second_target == NULL
        ? (ComputedStyle) {0}
        : style_for_node(&second_sheet, second_target, NULL);
    ok = ok && second_style.display == DISPLAY_FLEX
        && second_style.color == 0xfedcba
        && second_stats.compiled_fragment_hits == 1
        && second_stats.compiled_fragment_misses == 0
        && second_stats.compiled_fragment_rules_reused >= 2
        && second_stats.parsed_ir_hits == 1
        && second_stats.parsed_ir_misses == 0
        && second_stats.parsed_ir_operations_reused >= 2;
    if (!ok) {
        fprintf(stderr,
                "compiled fragment first=%zu/%zu/%zu second=%zu/%zu "
                "rules=%zu ir=%zu/%zu/%zu:%zu/%zu/%zu/%zu "
                "colors=%06x/%06x display=%d/%d\n",
                first_stats.compiled_fragment_hits,
                first_stats.compiled_fragment_misses,
                first_stats.compiled_fragment_stores,
                second_stats.compiled_fragment_hits,
                second_stats.compiled_fragment_misses,
                second_stats.compiled_fragment_rules_reused,
                first_stats.parsed_ir_hits,
                first_stats.parsed_ir_misses,
                first_stats.parsed_ir_stores,
                second_stats.parsed_ir_hits,
                second_stats.parsed_ir_misses,
                second_stats.parsed_ir_stores,
                second_stats.parsed_ir_operations_reused,
                first_style.color, second_style.color,
                first_style.display, second_style.display);
    }
    stylesheet_destroy(&second_sheet);
    document_destroy(&second_document);
    browser_session_destroy(&session);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_parsed_ir_is_viewport_bound_and_fails_closed(void)
{
    static const char html[] =
        "<!doctype html><div id=target class=wide>probe</div>";
    static const char css[] =
        "/* A retained comment makes this fixture representative of a "
        "source whose structural form is smaller than its source text. */"
        "@media (min-width:400px){.wide{display:flex}}";
    static const char layered_css[] =
        "@layer base{.wide{display:flex}}";
    static const char compact_css[] = ".wide{display:flex}";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    bool installed = budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet wide = {0}, narrow = {0}, layered = {0};
    unsigned char *ir = NULL, *layered_ir = NULL, *compact_ir = NULL;
    size_t ir_length = 0, layered_ir_length = 0, compact_ir_length = 0;
    bool ok = installed
        && document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
        && stylesheet_build(&wide, &budget, &document, 480)
        && stylesheet_add_css_from_context_capture_ir(
               &wide, css, sizeof(css) - 1u, "https://fixture.test/a.css",
               "", &ir, &ir_length)
        && ir != NULL && ir_length != 0
        && stylesheet_parsed_ir_matches(&wide, ir, ir_length)
        && stylesheet_build(&narrow, &budget, &document, 320)
        && !stylesheet_parsed_ir_matches(&narrow, ir, ir_length);
    if (ok) {
        unsigned char saved = ir[0];
        ir[0] ^= 0xffu;
        ok = !stylesheet_parsed_ir_matches(&wide, ir, ir_length);
        ir[0] = saved;
    }
    ok = ok && stylesheet_build(&layered, &budget, &document, 480)
        && stylesheet_add_css_from_context_capture_ir(
               &layered, layered_css, sizeof(layered_css) - 1u,
               "https://fixture.test/layered.css", "", &layered_ir,
               &layered_ir_length)
        && layered_ir == NULL && layered_ir_length == 0
        && stylesheet_add_css_from_context_capture_ir(
               &wide, compact_css, sizeof(compact_css) - 1u,
               "https://fixture.test/compact.css", "", &compact_ir,
               &compact_ir_length)
        && compact_ir == NULL && compact_ir_length == 0;
    budget_free(&budget, compact_ir);
    budget_free(&budget, layered_ir);
    budget_free(&budget, ir);
    stylesheet_destroy(&layered);
    stylesheet_destroy(&narrow);
    stylesheet_destroy(&wide);
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_complete_selector_census_reopens_retained_sources_once(void)
{
    BrowserSharedBody retained = {0};
    StylesheetDocumentResources resources = {0};
    resources.count = 2;
    resources.final_resample_required = true;
    resources.items[0].state = STYLESHEET_DOCUMENT_RESOURCE_LOADED;
    resources.items[0].body = &retained;
    resources.items[0].response_provenance_known = true;
    resources.items[0].rules_applied = true;
    resources.items[1].state = STYLESHEET_DOCUMENT_RESOURCE_LOADED;
    resources.items[1].body = &retained;
    resources.items[1].response_provenance_known = false;
    resources.items[1].rules_applied = true;
    bool rebuild = stylesheet_document_resources_prepare_complete_census(
        &resources);
    bool second = stylesheet_document_resources_prepare_complete_census(
        &resources);
    return rebuild && !second && resources.selector_census_complete
        && resources.final_resample_completed
        && !resources.final_resample_required
        && !resources.items[0].rules_applied
        && resources.items[1].rules_applied;
}

int main(void)
{
#define RUN_TEST(test) do {                                                  \
    if (!(test)()) {                                                         \
        fprintf(stderr, "stylesheet resource test failed: %s\n", #test);   \
        return 1;                                                            \
    }                                                                        \
} while (0)
    RUN_TEST(test_nonmatching_media_settles_without_applying);
    RUN_TEST(test_integrity_mismatch_rejects_stylesheet);
    RUN_TEST(test_integrity_is_scoped_to_each_link_element);
    RUN_TEST(test_integrity_rechecks_cross_origin_redirect);
    RUN_TEST(test_matching_duplicate_promotes_queued_response);
    RUN_TEST(test_inactive_declared_themes_do_not_consume_quota);
    RUN_TEST(test_initial_link_uses_document_context_and_attribute_policy);
    RUN_TEST(test_author_rules_follow_document_input_order);
    RUN_TEST(test_inline_import_precedes_its_local_rules);
    RUN_TEST(test_http_error_is_terminal);
    RUN_TEST(test_redirected_stylesheet_base_survives_retention);
    RUN_TEST(test_absent_response_policy_uses_default_for_import);
    RUN_TEST(test_stale_304_merges_response_policy);
    RUN_TEST(test_unknown_cache_provenance_is_bypassed);
    RUN_TEST(test_css_images_keep_declaring_source_context);
    RUN_TEST(test_css_data_svg_mask_uses_bounded_image_pipeline);
    RUN_TEST(test_webp_url_uses_same_origin_jpeg_sibling);
    RUN_TEST(test_css_paint_layers_keep_distinct_resources);
    RUN_TEST(test_invalid_href_has_terminal_state);
    RUN_TEST(test_blocker_bypasses_fresh_cached_stylesheet);
    RUN_TEST(test_cors_stylesheet_uses_partitioned_resource_cache);
    RUN_TEST(test_cors_stylesheet_reuses_document_preload);
    RUN_TEST(test_stylesheet_pressure_omits_optional_sheet);
    RUN_TEST(test_pressure_skipped_sheet_preserves_later_byte_quota);
    RUN_TEST(test_compiled_fragment_reuses_external_css_with_new_inline_css);
    RUN_TEST(test_parsed_ir_is_viewport_bound_and_fails_closed);
    RUN_TEST(test_complete_selector_census_reopens_retained_sources_once);
#undef RUN_TEST
    puts("stylesheet resource tests passed");
    return 0;
}
