/* Drives test_http2_redirect_reuse.py: through one fetch scheduler (one
   libcurl multi, so one shared connection), GET /first, then POST /post and
   follow its 302 to /target, twice. The redirect hop is the second stream
   or later on its connection; an aborted redirect stream must leave that
   connection usable. Then a redirect that ends with a set-cookie trailer
   (a trailer is not a header: its cookie is never staged) and one whose
   body is over the 64 KiB drain bound (the transfer stops and follows
   Location all the same). Exit 0 when every request succeeds over HTTP/2,
   1 on a failure, 77 when the transport did not negotiate HTTP/2. */
#include <stdio.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"

static int fetch_one(FetchScheduler *scheduler, Budget *budget,
                     const char *url, const FetchRequest *request,
                     const char *expected_url, bool *http2)
{
    uint64_t id = fetch_scheduler_enqueue(
        scheduler, url, request, 64u * 1024u, 10000);
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
        bool ok = success && result.status_code == 200
            && strcmp(result.effective_url, expected_url) == 0;
        for (size_t i = 0; i < result.set_cookie_count; i++) {
            if (result.set_cookies[i] != NULL
                && strstr(result.set_cookies[i], "trailer=") != NULL) {
                fprintf(stderr, "%s staged a trailer cookie: %s\n", url,
                        result.set_cookies[i]);
                ok = false;
            }
        }
        *http2 = *http2 && strcmp(fetch_http_version_name(
            result.negotiated_http_version), "h2") == 0;
        printf("%s %s status=%ld version=%s effective=%s error=%s\n",
               request->method, url, result.status_code,
               fetch_http_version_name(result.negotiated_http_version),
               result.effective_url, result.error);
        fetch_result_free(&result);
        return ok ? 0 : 1;
    }
    fprintf(stderr, "%s did not finish\n", url);
    return 1;
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
    char first[256], post[256], trailer[256], large[256], target[256];
    snprintf(first, sizeof(first), "%s/first", argv[1]);
    snprintf(post, sizeof(post), "%s/post", argv[1]);
    snprintf(trailer, sizeof(trailer), "%s/post-trailer", argv[1]);
    snprintf(large, sizeof(large), "%s/post-large", argv[1]);
    snprintf(target, sizeof(target), "%s/target", argv[1]);
    static char body[3072];
    memset(body, 'a', sizeof(body));
    const FetchRequest get = {.method = "GET"};
    const FetchRequest submit = {
        .method = "POST", .body = body, .body_length = sizeof(body),
        .content_type = "application/x-www-form-urlencoded"
    };
    bool http2 = true;
    int failures = 0;
    for (int round = 0; round < 2; round++) {
        failures += fetch_one(scheduler, &budget, first, &get, first, &http2);
        failures += fetch_one(scheduler, &budget, post, &submit, target,
                              &http2);
    }
    failures += fetch_one(scheduler, &budget, trailer, &submit, target,
                          &http2);
    failures += fetch_one(scheduler, &budget, large, &submit, target,
                          &http2);
    failures += fetch_one(scheduler, &budget, first, &get, first, &http2);
    fetch_scheduler_destroy(scheduler);
    if (budget.current != 0) {
        fprintf(stderr, "budget leak: %zu\n", budget.current);
        failures++;
    }
    if (failures == 0 && !http2) return 77;
    return failures == 0 ? 0 : 1;
}
