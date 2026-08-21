#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"
#include "tilefinch/script_loader.h"
#include "tilefinch/script_lazy.h"
#include "tilefinch/session.h"
#include "tilefinch/url.h"
#include "tilefinch/user_agent.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MIB (1024u * 1024u)

#define REQUIRE(condition) do {                                             \
    if (!(condition)) {                                                     \
        fprintf(stderr, "%s failed at %s:%d: %s\n",                       \
                test_name, __FILE__, __LINE__, #condition);                 \
        ok = false;                                                         \
        goto cleanup;                                                       \
    }                                                                       \
} while (0)

static const char runtime_page[] =
    "<!doctype html><title>Dynamic script task test</title><body>"
    "<script>globalThis.pocSummary='ready';</script></body>";

static const char quota_module_source[] =
    "import {quotaValue} from './quota-shared.js';"
    "import {quotaValue as duplicateValue} from './quota-shared.js';"
    "globalThis.moduleQuotaValue=quotaValue+duplicateValue;";

static const char quota_module_dependency_source[] =
    "export const quotaValue=7;";

static const char append_quota_module[] =
    "globalThis.moduleQuotaValue=0;"
    "const quotaModule=document.createElement('script');"
    "quotaModule.type='module';quotaModule.src='/quota-root.js';"
    "quotaModule.addEventListener('load',()=>{globalThis.pocSummary="
    "'module-quota:load:'+globalThis.moduleQuotaValue;});"
    "quotaModule.addEventListener('error',()=>{globalThis.pocSummary="
    "'module-quota:error:'+globalThis.moduleQuotaValue;});"
    "document.head.appendChild(quotaModule);"
    "globalThis.pocSummary='module-quota:pending';";

typedef struct {
    const char *url;
    const char *source;
    const char *referrer_url;
    const char *referrer_policy;
    const char *response_referrer_policy;
    size_t delay_pumps;
    bool external_cancel;
    bool transport_failure;
    bool module;
    bool validate_request_context;
} ScriptReplayRecord;

static uint64_t replay_body_hash(const char *data, size_t length)
{
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++) {
        value ^= (unsigned char) data[i];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static bool write_module_replay_metadata(
    FILE *meta, const ScriptReplayRecord *record, size_t length)
{
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char document_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (meta == NULL || record == NULL
        || !tilefinch_url_origin(record->url, origin, sizeof(origin))) {
        return false;
    }
    int document_length = snprintf(
        document_url, sizeof(document_url), "%s/", origin);
    if (document_length <= 0
        || (size_t) document_length >= sizeof(document_url)) return false;
    const char *referrer_source = record->referrer_url == NULL
        ? document_url : record->referrer_url;
    const char *referrer_policy = record->referrer_policy == NULL
        ? "" : record->referrer_policy;
    bool contextual = record->module || record->validate_request_context;
    char referer[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!fetch_compute_referrer(
            referrer_source, record->url, referrer_policy,
            referer, sizeof(referer))) return false;
    bool success = !record->transport_failure;
    uint64_t body_hash = replay_body_hash(
        success ? record->source : "", success ? length : 0);
    bool written = fprintf(
        meta,
        "psp-http-trace=10\n"
        "cookie-values=redacted\n"
        "method=GET\nurl=%s\nlogical-request-url=%s\nsuccess=%d\n"
        "async-delay-pumps=%zu\nexternal-cancel=%d\n"
        "transport-timeout=0\nredirect-origin-tainted=0\nerror=%s\n"
        "request-body-length=0\n"
        "request-body-hash=cbf29ce484222325\n"
        "request-content-type=\n"
        "request-cookie-bytes=0\nrequest-has-cf-clearance=0\n"
        "request-extra-header-bytes=0\nrequest-extra-header-shape=\n"
        "request-allow-http-errors=1\nrequest-enforce-cors=%d\n"
        "request-redirect-same-origin-only=0\n"
        "request-cors-cached-response-validated=0\n"
        "request-if-none-match=\nrequest-if-modified-since=\n"
        "request-referer=%s\nrequest-origin=%s\nrequest-accept=%s\n"
        "request-sec-fetch-dest=%s\nrequest-sec-fetch-mode=%s\n"
        "request-sec-fetch-site=%s\n"
        "request-send-client-hints=0\nrequest-client-hint-tokens=\n"
        "request-client-hint-origin=\nrequest-send-low-client-hints=1\n"
        "request-sec-fetch-user=0\nrequest-upgrade-insecure=0\n"
        "request-user-agent=" TILEFINCH_BROWSER_USER_AGENT "\n"
        "request-diagnostic-mobile-safari=0\nrequest-credentials=%d\n"
        "request-credential-origin=%s\nrequest-initiator-url=%s\n"
        "request-referrer-source=%s\nrequest-referrer-policy=%s\n"
        "status=%d\nlength=%zu\nresponse-body-hash=%016llx\n"
        "effective-url=%s\ncontent-type=%s\netag=\nlast-modified=\n"
        "cf-mitigated=\naccept-ch=\ncritical-ch=\nserver=%s\ncf-ray=\n"
        "response-referrer-policy-metadata-valid=1\n"
        "response-referrer-policy-present=%d\n"
        "response-referrer-policy=%s\n"
        "response-security-headers-truncated=0\n"
        "response-header-count=%d\nset-cookie-count=0\n",
        record->url, record->url, success ? 1 : 0, record->delay_pumps,
        record->external_cancel ? 1 : 0,
        success ? "" : "fixture transport failure",
        record->module ? 1 : 0, contextual ? referer : "",
        record->module ? origin : "",
        contextual ? "*/*" : "",
        contextual ? "script" : "",
        record->module ? "cors" : contextual ? "no-cors" : "",
        contextual ? "same-origin" : "",
        record->module ? 2 : 0,
        contextual ? (record->module ? origin : document_url) : "",
        contextual ? document_url : "",
        contextual ? referrer_source : "",
        contextual ? referrer_policy : "",
        success ? 200 : 0, length,
        (unsigned long long) body_hash, success ? record->url : "",
        success ? "text/javascript" : "", success ? "fixture" : "",
        record->response_referrer_policy == NULL ? 0 : 1,
        record->response_referrer_policy == NULL
            ? "" : record->response_referrer_policy,
        success ? (record->response_referrer_policy == NULL ? 1 : 2) : 0)
        > 0;
    if (written && success) {
        written = fprintf(
            meta, "response-header-0=content-type: text/javascript\n") > 0;
    }
    if (written && success && record->response_referrer_policy != NULL) {
        written = fprintf(
            meta, "response-header-1=referrer-policy: %s\n",
            record->response_referrer_policy) > 0;
    }
    return written;
}

static bool write_replay_fixture(
    char directory[128], const ScriptReplayRecord *records, size_t count)
{
    snprintf(directory, 128, "%s", "/tmp/tilefinch-dynamic-script-XXXXXX");
    if (mkdtemp(directory) == NULL) return false;
    bool ok = true;
    for (size_t i = 0; ok && i < count; i++) {
        char body_path[192], meta_path[192];
        ok = snprintf(body_path, sizeof(body_path), "%s/%04zu.body",
                      directory, i) > 0
            && snprintf(meta_path, sizeof(meta_path), "%s/%04zu.meta",
                        directory, i) > 0;
        FILE *body = ok ? fopen(body_path, "wb") : NULL;
        size_t length = records[i].transport_failure
            ? 0 : strlen(records[i].source);
        bool body_written = body != NULL
            && fwrite(records[i].source, 1, length, body) == length;
        bool body_closed = body != NULL && fclose(body) == 0;
        if (!body_written || !body_closed) {
            ok = false;
            break;
        }
        FILE *meta = fopen(meta_path, "wb");
        if (meta == NULL) { ok = false; break; }
        bool written = write_module_replay_metadata(
            meta, &records[i], length);
        ok = written && fclose(meta) == 0;
    }
    char clock_path[192];
    if (ok && snprintf(clock_path, sizeof(clock_path), "%s/trace.meta",
                       directory) > 0) {
        FILE *clock = fopen(clock_path, "wb");
        ok = clock != NULL
            && fprintf(clock,
                       "psp-http-trace-clock=1\norigin-ms=1000\n") > 0
            && fclose(clock) == 0;
    } else ok = false;
    return ok;
}

static void remove_replay_fixture(const char *directory, size_t count)
{
    if (directory == NULL || directory[0] == '\0') return;
    char path[192];
    for (size_t i = 0; i < count; i++) {
        if (snprintf(path, sizeof(path), "%s/%04zu.body", directory, i) > 0) {
            (void) unlink(path);
        }
        if (snprintf(path, sizeof(path), "%s/%04zu.meta", directory, i) > 0) {
            (void) unlink(path);
        }
    }
    if (snprintf(path, sizeof(path), "%s/trace.meta", directory) > 0) {
        (void) unlink(path);
    }
    (void) rmdir(directory);
}

static bool start_runtime_page_with_heap_and_timeout(
    NavigationSession *navigation, Budget *budget,
    BrowserSession *browser, const char *url, size_t heap_bytes,
    unsigned timeout_ms)
{
    if (!navigation_init(navigation, budget, 4)) return false;
    navigation_attach_browser_session(navigation, browser);
    navigation_enable_scripts(navigation, heap_bytes, timeout_ms);
    navigation_enable_document_scripts(
        navigation, 8, 64u * 1024u, 32u * 1024u,
        (long) timeout_ms);
    uint64_t generation = navigation_begin(navigation);
    return navigation_commit_html(
        navigation, generation, url, runtime_page, sizeof(runtime_page) - 1u,
        480, NULL, NULL, true);
}

static bool start_runtime_page_with_timeout(
    NavigationSession *navigation, Budget *budget,
    BrowserSession *browser, const char *url, unsigned timeout_ms)
{
    return start_runtime_page_with_heap_and_timeout(
        navigation, budget, browser, url, 8u * MIB, timeout_ms);
}

static bool start_runtime_page(NavigationSession *navigation, Budget *budget,
                               BrowserSession *browser, const char *url)
{
    return start_runtime_page_with_timeout(
        navigation, budget, browser, url, 1000);
}

static bool start_custom_runtime_page(
    NavigationSession *navigation, Budget *budget, BrowserSession *browser,
    const char *url, const char *html, size_t maximum_scripts,
    size_t maximum_total_bytes, size_t maximum_file_bytes)
{
    if (!navigation_init(navigation, budget, 4)) return false;
    navigation_attach_browser_session(navigation, browser);
    navigation_enable_scripts(navigation, 8u * MIB, 1000);
    navigation_enable_document_scripts(
        navigation, maximum_scripts, maximum_total_bytes,
        maximum_file_bytes, 1000);
    uint64_t generation = navigation_begin(navigation);
    return navigation_commit_html(
        navigation, generation, url, html, strlen(html), 480,
        NULL, NULL, true);
}

static bool evaluate(NavigationSession *navigation, const char *source,
                     const char *name)
{
    return navigation->page.runtime != NULL
        && script_runtime_evaluate_diagnostic(
               navigation->page.runtime, source, name,
               &navigation->page.script_result);
}

static bool advance_until(NavigationSession *navigation, const char *summary,
                          size_t maximum_advances, size_t *advances)
{
    for (size_t step = 0; step <= maximum_advances; step++) {
        if (strcmp(navigation->page.script_result.summary, summary) == 0) {
            if (advances != NULL) *advances = step;
            return true;
        }
        if (step == maximum_advances
            || !navigation_advance_runtime(navigation, 1, 1)) {
            fprintf(stderr,
                    "advance exhausted: expected=\"%s\" actual=\"%s\" "
                    "error=\"%s\" pending=%zu failures=%zu status=%ld "
                    "url=\"%s\" type=\"%s\"\n",
                    summary, navigation->page.script_result.summary,
                    navigation->page.script_result.error,
                    navigation->page.script_result.pending_tasks,
                    navigation->page.script_result.network_failures,
                    navigation->page.script_result.last_network_status,
                    navigation->page.script_result.last_network_url,
                    navigation->page.script_result.last_network_content_type);
            return false;
        }
    }
    return false;
}

static bool put_script(BrowserSession *browser, const char *url,
                       const char *source)
{
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char document_url[TILEFINCH_ORIGIN_SERIALIZED_LIMIT + 2u];
    size_t source_length = source == NULL ? 0 : strlen(source);
    if (browser == NULL || source_length == 0
        || !tilefinch_url_origin(url, origin, sizeof(origin))) return false;
    int written = snprintf(document_url, sizeof(document_url), "%s/", origin);
    if (written <= 0 || (size_t) written >= sizeof(document_url)) return false;
    unsigned char *copy = budget_malloc(
        browser->budget, source_length + 1u);
    if (copy == NULL) return false;
    memcpy(copy, source, source_length);
    copy[source_length] = 0;
    BrowserSharedBody *body = browser_shared_body_take(
        browser->budget, copy, source_length);
    if (body == NULL) {
        budget_free(browser->budget, copy);
        return false;
    }
    TilefinchRequestContext context = {
        .target_url = url, .initiator_url = document_url,
        .top_level_url = document_url, .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_SCRIPT
    };
    TilefinchResourceGrant grant = {
        .destination = TILEFINCH_DESTINATION_SCRIPT,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .final_same_origin = true
    };
    bool stored = browser_session_cache_put_http_shared_classic_script(
        browser, url, body, NULL, NULL, "text/javascript",
        "immutable", NULL, 0, &context, &grant);
    browser_shared_body_release(body);
    return stored;
}

static bool put_module_policy(BrowserSession *browser, const char *url,
                              const char *source,
                              const char *response_referrer_policy)
{
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (!tilefinch_url_origin(url, origin, sizeof(origin))) return false;
    BrowserModuleCacheProvenance provenance = {
        .effective_url = url,
        .initiator_origin = origin,
        .top_level_url = url,
        .response_referrer_policy = response_referrer_policy,
        .credentials = TILEFINCH_CREDENTIALS_SAME_ORIGIN,
        .cors_validated = true,
        .javascript_mime_validated = true,
        .referrer_policy_header_present =
            response_referrer_policy != NULL
    };
    return browser_session_cache_put_http_module(
        browser, url, (const unsigned char *) source, strlen(source),
        NULL, NULL, "text/javascript", "immutable", NULL, 0,
        &provenance);
}

static bool put_module(BrowserSession *browser, const char *url,
                       const char *source)
{
    return put_module_policy(browser, url, source, NULL);
}

static char *make_padded_script_source(const char *prefix, size_t length)
{
    size_t prefix_length = prefix == NULL ? 0 : strlen(prefix);
    if (prefix == NULL || prefix_length + 4u > length
        || length == SIZE_MAX) return NULL;
    char *source = malloc(length + 1u);
    if (source == NULL) return NULL;
    memcpy(source, prefix, prefix_length);
    memcpy(source + prefix_length, "/*", 2);
    memset(source + prefix_length + 2u, ' ',
           length - prefix_length - 4u);
    memcpy(source + length - 2u, "*/", 2);
    source[length] = '\0';
    return source;
}

/* Keep runtime/cache fixtures process-local while separating dynamic-script
   ordering, lifecycle, quota, and native-state scenarios. */
#include "suites/dynamic_script_ordering.inc"
#include "suites/dynamic_script_lifecycle.inc"
#include "suites/dynamic_script_quota.inc"
#include "suites/dynamic_script_state.inc"
#include "suites/dynamic_script_runner.inc"
