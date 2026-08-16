#include "tilefinch/budget.h"
#include "tilefinch/browser_engine.h"
#include "tilefinch/document.h"
#include "tilefinch/fetch.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"
#include "tilefinch/resources.h"
#include "tilefinch/session.h"
#include "tilefinch/style.h"

#include "../src/fetch_redirect_abort.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB (1024u * 1024u)

_Static_assert(sizeof(FetchResult) < 16u * 1024u,
               "FetchResult must not inline worst-case response cookies");

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "redirect-test failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

typedef struct {
    char body[512];
    size_t length;
    size_t header_calls;
} StreamProbe;

typedef struct {
    size_t header_calls;
    size_t body_bytes;
    bool policy_ok;
} PolicyStreamProbe;

static bool stream_headers(void *opaque, const FetchResult *metadata)
{
    StreamProbe *probe = opaque;
    probe->header_calls++;
    return metadata->status_code == 200
        && strcmp(metadata->etag, "\"final\"") == 0
        && metadata->critical_ch[0] == '\0';
}

static bool stream_body(void *opaque, const unsigned char *data,
                        size_t length)
{
    StreamProbe *probe = opaque;
    if (length >= sizeof(probe->body) - probe->length) return false;
    memcpy(probe->body + probe->length, data, length);
    probe->length += length;
    probe->body[probe->length] = '\0';
    return true;
}

static bool count_headers(void *opaque, const FetchResult *metadata)
{
    (void) metadata;
    StreamProbe *probe = opaque;
    probe->header_calls++;
    return true;
}

static bool policy_stream_headers(void *opaque, const FetchResult *metadata)
{
    PolicyStreamProbe *probe = opaque;
    bool present = false;
    char policy[FETCH_REFERRER_POLICY_LIMIT];
    probe->header_calls++;
    probe->policy_ok = fetch_response_referrer_policy(
        metadata, &present, policy)
        && present && strcmp(policy, "no-referrer") == 0
        && strstr(metadata->response_headers, "referrer-policy:") == NULL;
    return probe->policy_ok;
}

static bool policy_stream_body(void *opaque, const unsigned char *data,
                               size_t length)
{
    (void) data;
    PolicyStreamProbe *probe = opaque;
    probe->body_bytes += length;
    return true;
}

static bool fetch_redirect(Budget *budget, const char *url,
                           const FetchRequest *request, FetchResult *result)
{
    return fetch_request_cancelable(budget, url, request, 64 * 1024, 5000,
                                    NULL, NULL, result);
}

static bool fetch_scheduled(Budget *budget, const char *url,
                            const FetchRequest *request, FetchResult *result)
{
    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64 * 1024);
    if (scheduler == NULL) return false;
    uint64_t id = fetch_scheduler_enqueue(
        scheduler, url, request, 64 * 1024, 5000);
    bool success = false, taken = false;
    for (size_t pump = 0; id != 0 && pump < 2000 && !taken; pump++) {
        (void) fetch_scheduler_pump(scheduler, 1, 4);
        taken = fetch_scheduler_take(scheduler, id, &success, result);
    }
    fetch_scheduler_destroy(scheduler);
    return id != 0 && taken && success;
}

static bool response_cookie_storage_empty(const FetchResult *result)
{
    if (result == NULL || result->set_cookie_count != 0) return false;
    for (size_t i = 0; i < FETCH_RESPONSE_COOKIE_CAPACITY; i++) {
        if (result->set_cookies[i] != NULL
            || result->set_cookie_urls[i] != NULL) return false;
    }
    return true;
}

static bool response_cookie_urls_are_fragmentless(const FetchResult *result)
{
    if (result == NULL) return false;
    for (size_t i = 0; i < result->set_cookie_count; i++) {
        const char *url = fetch_set_cookie_url(result, i, NULL);
        if (url == NULL || strchr(url, '#') != NULL) return false;
    }
    return true;
}

/* Preserve one process and its shared replay/session fixtures while keeping
   the redirect, trace, and page-script scenarios independently readable. */
#include "suites/fetch_redirect_policy.inc"
#include "suites/fetch_redirect_cookies.inc"
#include "suites/fetch_redirect_methods.inc"
#include "suites/fetch_redirect_trace.inc"
#include "suites/fetch_redirect_script.inc"
#include "suites/fetch_redirect_runner.inc"
