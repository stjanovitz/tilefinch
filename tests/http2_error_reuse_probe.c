/* Drives test_http2_redirect_reuse.py's "error" case: a rejected HTTP error
   response must leave its shared HTTP/2 connection usable. Through one fetch
   scheduler (one libcurl multi, so one shared connection), GET /fields, a
   404 with no body, /fields, a 404 with a body, /fields; then the same five
   through the synchronous transport (which opens a connection per request,
   so it checks only how the 404s are rejected). Each 404 must fail as an
   HTTP error; each /fields must arrive whole, with exactly the field names
   and values the server sent and its own cookie. Exit 0 when every request
   behaves, 1 on a failure, 77 when the transport did not negotiate
   HTTP/2. */
#include <stdio.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"

static const char *const expected_fields[] = {
    "content-type: text/html; charset=utf-8\n",
    "x-frame-options: DENY\n",
    "cache-control: no-cache, no-store, max-age=0, must-revalidate\n",
    "x-content-type-options: nosniff\n",
};

/* /fields's headers hold the server's fields and nothing else: a client
   that decodes a header name from the wrong HPACK table entry shows a
   duplicated or misnamed field, or a cookie that is not this stream's. */
static int check_fields(const char *url, const FetchResult *result)
{
    int failures = 0;
    const char *headers = result->response_headers;
    for (size_t i = 0;
         i < sizeof(expected_fields) / sizeof(expected_fields[0]); i++) {
        const char *at = strstr(headers, expected_fields[i]);
        if (at == NULL
            || strstr(at + 1, expected_fields[i]) != NULL) {
            fprintf(stderr, "%s: field %.*s is missing or repeated\n", url,
                    (int) strlen(expected_fields[i]) - 1,
                    expected_fields[i]);
            failures++;
        }
    }
    static const char *const names[] = {
        "content-type", "x-frame-options", "cache-control",
        "x-content-type-options", "x-request-id", "content-length", "date",
    };
    size_t lines = 0;
    for (const char *line = headers; *line != '\0'; lines++) {
        const char *end = strchr(line, '\n');
        size_t length = end == NULL ? strlen(line) : (size_t) (end - line);
        const char *colon = memchr(line, ':', length);
        bool known = false;
        for (size_t i = 0; colon != NULL
                           && i < sizeof(names) / sizeof(names[0]); i++) {
            known = known
                || ((size_t) (colon - line) == strlen(names[i])
                    && strncmp(line, names[i], strlen(names[i])) == 0);
        }
        if (!known) {
            fprintf(stderr, "%s: unexpected field %.*s\n", url,
                    (int) length, line);
            failures++;
        }
        line = end == NULL ? line + length : end + 1;
    }
    if (lines != sizeof(names) / sizeof(names[0])) {
        fprintf(stderr, "%s: %zu fields, expected %zu\n", url, lines,
                sizeof(names) / sizeof(names[0]));
        failures++;
    }
    const char *id = strstr(headers, "x-request-id: r");
    char cookie[64] = "";
    if (id != NULL) {
        const char *value = id + strlen("x-request-id: ");
        size_t length = strcspn(value, "\n");
        snprintf(cookie, sizeof(cookie), "seen=%.*s; Path=/", (int) length,
                 value);
    }
    if (id == NULL || result->set_cookie_count != 1
        || result->set_cookies[0] == NULL
        || strcmp(result->set_cookies[0], cookie) != 0) {
        fprintf(stderr, "%s: cookie %s does not match its x-request-id"
                        " (expected %s)\n", url,
                result->set_cookie_count == 1 && result->set_cookies[0]
                    ? result->set_cookies[0] : "(none)",
                cookie[0] != '\0' ? cookie : "(no x-request-id)");
        failures++;
    }
    return failures == 0 ? 0 : 1;
}

static int check_result(const char *url, bool success,
                        const FetchResult *result, bool *http2)
{
    bool missing = strstr(url, "/missing") != NULL;
    printf("%s success=%d status=%ld version=%s error=%s\n", url,
           (int) success, result->status_code,
           fetch_http_version_name(result->negotiated_http_version),
           result->error);
    /* The synchronous transport clears a failed result, its status and
       version too, so only a result that kept its status says which
       protocol carried it. */
    if (result->status_code != 0)
        *http2 = *http2 && strcmp(fetch_http_version_name(
            result->negotiated_http_version), "h2") == 0;
    if (missing) {
        bool ok = !success
            && (result->status_code == 404 || result->status_code == 0)
            && strstr(result->error, "returned error: 404") != NULL;
        if (!ok) fprintf(stderr, "%s was not rejected as a 404\n", url);
        return ok ? 0 : 1;
    }
    if (!success || result->status_code != 200) {
        fprintf(stderr, "%s failed\n", url);
        return 1;
    }
    return check_fields(url, result);
}

static int fetch_scheduled(FetchScheduler *scheduler, Budget *budget,
                           const char *url, bool *http2)
{
    const FetchRequest get = {.method = "GET"};
    uint64_t id = fetch_scheduler_enqueue(
        scheduler, url, &get, 64u * 1024u, 10000);
    if (id == 0) {
        fprintf(stderr, "enqueue %s: %s\n", url,
                fetch_scheduler_last_error(scheduler));
        return 1;
    }
    for (int turn = 0; turn < 2000; turn++) {
        (void) fetch_scheduler_pump(scheduler, 4, 10);
        bool success = false;
        FetchResult result = {.budget = budget};
        if (!fetch_scheduler_take(scheduler, id, &success, &result))
            continue;
        int failed = check_result(url, success, &result, http2);
        fetch_result_free(&result);
        return failed;
    }
    fprintf(stderr, "%s did not finish\n", url);
    return 1;
}

static int fetch_direct(Budget *budget, const char *url, bool *http2)
{
    const FetchRequest get = {.method = "GET"};
    FetchResult result = {.budget = budget};
    bool success = fetch_request_cancelable(
        budget, url, &get, 64u * 1024u, 10000, NULL, NULL, &result);
    int failed = check_result(url, success, &result, http2);
    fetch_result_free(&result);
    return failed;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s BASE-URL CA-PEM\n", argv[0]);
        return 2;
    }
    if (!fetch_set_ca_bundle_path(argv[2])) return 2;
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    FetchScheduler *scheduler =
        fetch_scheduler_create(&budget, 2, 512u * 1024u);
    if (scheduler == NULL) return 2;
    char fields[256], missing[256], missing_body[256];
    snprintf(fields, sizeof(fields), "%s/fields", argv[1]);
    snprintf(missing, sizeof(missing), "%s/missing", argv[1]);
    snprintf(missing_body, sizeof(missing_body), "%s/missing-body",
             argv[1]);
    bool http2 = true;
    int failures = 0;
    failures += fetch_scheduled(scheduler, &budget, fields, &http2);
    failures += fetch_scheduled(scheduler, &budget, missing, &http2);
    failures += fetch_scheduled(scheduler, &budget, fields, &http2);
    failures += fetch_scheduled(scheduler, &budget, missing_body, &http2);
    failures += fetch_scheduled(scheduler, &budget, fields, &http2);
    fetch_scheduler_destroy(scheduler);
    failures += fetch_direct(&budget, fields, &http2);
    failures += fetch_direct(&budget, missing, &http2);
    failures += fetch_direct(&budget, fields, &http2);
    failures += fetch_direct(&budget, missing_body, &http2);
    failures += fetch_direct(&budget, fields, &http2);
    if (budget.current != 0) {
        fprintf(stderr, "budget leak: %zu\n", budget.current);
        failures++;
    }
    if (failures == 0 && !http2) return 77;
    return failures == 0 ? 0 : 1;
}
