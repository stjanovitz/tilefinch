static bool test_cross_host_cookie_isolation(Budget *budget, unsigned port)
{
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/cross", port);
    FetchRequest request = {
        .method = "GET", .cookie = "secret=initial",
        .extra_headers =
            "Authorization: Bearer must-not-cross\nX-Public: retained",
        .referer = "https://secure.example/source/path",
        .origin = "http://origin.example",
        .sec_fetch_site = "same-origin",
        .allow_http_errors = true,
        .initiator_url = url,
        .referrer_source = "https://secure.example/source/path",
        .referrer_policy = "strict-origin-when-cross-origin"
    };
    FetchResult result = {.budget = budget};
    CHECK(fetch_redirect(budget, url, &request, &result));
    CHECK(result.status_code == 200
          && strstr(result.effective_url, "http://localhost:") == result.effective_url
          && strstr(result.data, "cookie=<none>") != NULL
          && strstr(result.data, "secret=initial") == NULL
          && strstr(result.data, "origin_only=seen") == NULL
          && strstr(result.data, "authorization=<none>") != NULL
          && strstr(result.data, "origin=null") != NULL
          && strstr(result.data, "x-public=retained") != NULL
          && strstr(result.data, "referer=<none>") != NULL
          && strstr(result.data, "fetch-site=cross-site") != NULL
          && result.set_cookie_count == 2
          && strstr(fetch_set_cookie_url(&result, 0, ""),
                    "http://127.0.0.1:") == fetch_set_cookie_url(&result, 0, "")
          && strstr(fetch_set_cookie_url(&result, 1, ""),
                    "http://localhost:") == fetch_set_cookie_url(&result, 1, ""));
    fetch_result_destroy(&result);
    return true;
}

static bool test_policy_aware_redirect_cookies(Budget *budget,
                                               unsigned port)
{
    BrowserSession browser;
    CHECK(browser_session_init(&browser, budget, 64 * 1024));
    char initial[256], root[256], target[256];
    snprintf(initial, sizeof(initial),
             "http://127.0.0.1:%u/private/start", port);
    snprintf(root, sizeof(root), "http://127.0.0.1:%u/", port);
    snprintf(target, sizeof(target), "http://localhost:%u/", port);
    CHECK(browser_session_cookie_set_http(
              &browser, root, "id=base; Path=/")
          && browser_session_cookie_set_http(
              &browser, initial, "id=private; Path=/private")
          && browser_session_cookie_set_http(
              &browser, target, "target=eligible; Path=/"));

    TilefinchRequestContext context = {
        .target_url = initial,
        .initiator_url = initial,
        .top_level_url = initial,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_DOCUMENT,
        .top_level_navigation = true,
        .user_activated = true
    };
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true,
        .credentials = FETCH_CREDENTIALS_INCLUDE,
        .credential_origin = initial,
        .initiator_url = initial,
        .cookie_session = &browser,
        .cookie_context = &context
    };
    FetchResult result = {.budget = budget};
    CHECK(fetch_redirect(budget, initial, &request, &result)
          && strstr(result.data, "query=ordered=1") != NULL
          && strstr(result.data, "id=base") != NULL
          && strstr(result.data, "id=private") == NULL
          && strstr(result.data, "private_hop=seen") == NULL);
    fetch_result_destroy(&result);

    char cross[256];
    snprintf(cross, sizeof(cross),
             "http://127.0.0.1:%u/cross", port);
    context.target_url = cross;
    context.initiator_url = cross;
    context.top_level_url = cross;
    request.credential_origin = cross;
    request.initiator_url = cross;
    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64 * 1024);
    CHECK(scheduler != NULL);
    uint64_t id = fetch_scheduler_enqueue(
        scheduler, cross, &request, 64 * 1024, 5000);
    bool success = false, taken = false;
    result = (FetchResult) {.budget = budget};
    for (size_t pump = 0; id != 0 && pump < 2000 && !taken; pump++) {
        (void) fetch_scheduler_pump(scheduler, 1, 4);
        taken = fetch_scheduler_take(scheduler, id, &success, &result);
    }
    CHECK(id != 0 && taken && success
          && strstr(result.data, "target=eligible") != NULL
          && strstr(result.data, "origin_only=seen") == NULL);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);

    snprintf(cross, sizeof(cross),
             "http://127.0.0.1:%u/cross-cookie-policy", port);
    context.target_url = cross;
    context.initiator_url = cross;
    context.top_level_url = cross;
    request.credential_origin = cross;
    request.initiator_url = cross;
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_redirect(budget, cross, &request, &result)
          && strstr(result.data, "strict_hop=blocked") == NULL
          && strstr(result.data, "partitioned_hop=blocked") == NULL);
    fetch_result_destroy(&result);

    char unchanged[512];
    CHECK(browser_session_cookie_header(
              &browser, root, unchanged, sizeof(unchanged))
          && strstr(unchanged, "private_hop") == NULL
          && strstr(unchanged, "strict_hop") == NULL);
    browser_session_destroy(&browser);
    return true;
}

static bool test_same_origin_credentials_sticky_taint(Budget *budget,
                                                       unsigned port)
{
    char url[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cross-port", port);
    FetchRequest request = {
        .method = "GET", .cookie = "secret=initial",
        .allow_http_errors = true,
        .credentials = FETCH_CREDENTIALS_SAME_ORIGIN,
        .credential_origin = url,
        .initiator_url = url
    };
    FetchResult result = {.budget = budget};
    CHECK(fetch_redirect(budget, url, &request, &result));
    CHECK(result.status_code == 200
          && strstr(result.data, "cookie=<none>") != NULL
          && strstr(result.data, "secret=initial") == NULL
          && result.set_cookie_count == 1
          && strcmp(result.set_cookies[0],
                    "origin_only=seen; Path=/") == 0
          && strstr(fetch_set_cookie_url(&result, 0, ""),
                    "/cross-port") != NULL);
    fetch_result_destroy(&result);

    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cross-port-return", port);
    request.credential_origin = url;
    request.initiator_url = url;
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_redirect(budget, url, &request, &result));
    CHECK(result.status_code == 200
          && strstr(result.data, "query=cross_received=0") != NULL
          && strstr(result.data, "cookie=<none>") != NULL
          && strstr(result.data, "secret=initial") == NULL
          && strstr(result.data, "origin_only=seen") == NULL
          && strstr(result.data, "cross_only=blocked") == NULL
          && result.set_cookie_count == 1
          && strcmp(result.set_cookies[0],
                    "origin_only=seen; Path=/") == 0);
    fetch_result_destroy(&result);

    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cross-port", port);
    request.credential_origin = url;
    request.initiator_url = url;
    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64 * 1024);
    CHECK(scheduler != NULL);
    uint64_t id = fetch_scheduler_enqueue(
        scheduler, url, &request, 64 * 1024, 5000);
    bool success = false, taken = false;
    result = (FetchResult) {.budget = budget};
    for (size_t pump = 0; id != 0 && pump < 2000 && !taken; pump++) {
        (void) fetch_scheduler_pump(scheduler, 1, 4);
        taken = fetch_scheduler_take(scheduler, id, &success, &result);
    }
    CHECK(id != 0 && taken && success
          && strstr(result.data, "cookie=<none>") != NULL
          && result.set_cookie_count == 1);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    return true;
}

static bool test_omit_drops_response_cookies(Budget *budget, unsigned port)
{
    static const char *paths[] = {"final", "same"};
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true,
        .credentials = FETCH_CREDENTIALS_OMIT
    };
    for (size_t path = 0; path < sizeof(paths) / sizeof(paths[0]); path++) {
        char url[256];
        snprintf(url, sizeof(url), "http://127.0.0.1:%u/%s", port,
                 paths[path]);
        FetchResult result = {.budget = budget};
        CHECK(fetch_redirect(budget, url, &request, &result)
              && result.status_code == 200
              && result.set_cookie_count == 0);
        fetch_result_destroy(&result);

        FetchScheduler *scheduler = fetch_scheduler_create(
            budget, 1, 64 * 1024);
        CHECK(scheduler != NULL);
        uint64_t id = fetch_scheduler_enqueue(
            scheduler, url, &request, 64 * 1024, 5000);
        bool success = false, taken = false;
        result = (FetchResult) {.budget = budget};
        for (size_t pump = 0; id != 0 && pump < 2000 && !taken; pump++) {
            (void) fetch_scheduler_pump(scheduler, 1, 4);
            taken = fetch_scheduler_take(
                scheduler, id, &success, &result);
        }
        CHECK(id != 0 && taken && success
              && result.set_cookie_count == 0);
        fetch_result_destroy(&result);
        fetch_scheduler_destroy(scheduler);
    }
    return true;
}

static bool test_redirect_short_circuit_and_cookie_bounds(
    Budget *budget, unsigned port)
{
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true
    };
    char url[256];
    FetchResult result = {.budget = budget};
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/slow-redirect", port);
    CHECK(fetch_request_cancelable(
              budget, url, &request, 64 * 1024, 250, NULL, NULL, &result)
          && result.status_code == 200
          && strstr(result.data, "method=GET") != NULL);
    fetch_result_destroy(&result);

    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/max-cookie", port);
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_redirect(budget, url, &request, &result)
          && result.set_cookie_count == 1
          && strlen(result.set_cookies[0]) == 4095);
    fetch_result_destroy(&result);

    /* A sign-in chain sets the whole credential cookie set on several hops. */
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/redirect-cookie-chain", port);
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_redirect(budget, url, &request, &result)
          && result.status_code == 200
          && result.set_cookie_count >= 60u
          && !result.set_cookies_truncated);
    fetch_result_destroy(&result);

    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/response-cookie-capacity", port);
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_redirect(budget, url, &request, &result)
          && result.set_cookie_count == FETCH_RESPONSE_COOKIE_CAPACITY
          && !result.set_cookies_truncated);
    for (size_t i = 0; i < FETCH_RESPONSE_COOKIE_CAPACITY; i++) {
        char expected[64];
        int written = snprintf(expected, sizeof(expected),
                               "cookie%zu=value; Path=/", i);
        CHECK(written > 0 && (size_t) written < sizeof(expected)
              && result.set_cookies[i] != NULL
              && strcmp(result.set_cookies[i], expected) == 0
              && strstr(fetch_set_cookie_url(&result, i, ""),
                        "/response-cookie-capacity") != NULL);
    }
    fetch_result_destroy(&result);

    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/response-cookie-twenty", port);
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_redirect(budget, url, &request, &result)
          && result.set_cookie_count == 20
          && !result.set_cookies_truncated);
    fetch_result_destroy(&result);

    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/response-cookie-overflow", port);
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_redirect(budget, url, &request, &result)
          && result.set_cookie_count == FETCH_RESPONSE_COOKIE_CAPACITY
          && result.set_cookies_truncated);
    fetch_result_destroy(&result);

    static const char *rejected_paths[] = {
        "oversized-cookie", "redirect-cookie-overflow"
    };
    for (size_t i = 0;
         i < sizeof(rejected_paths) / sizeof(rejected_paths[0]); i++) {
        snprintf(url, sizeof(url), "http://127.0.0.1:%u/%s", port,
                 rejected_paths[i]);
        result = (FetchResult) {.budget = budget};
        CHECK(!fetch_redirect(budget, url, &request, &result)
              && response_cookie_storage_empty(&result)
              && (strstr(result.error, "cookie") != NULL
                  || strstr(result.error, "Cookie") != NULL));
        fetch_result_destroy(&result);
    }

    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64 * 1024);
    CHECK(scheduler != NULL);
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/slow-redirect", port);
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_scheduler_request(
              scheduler, url, &request, 64 * 1024, 250, &result)
          && result.status_code == 200);
    fetch_result_destroy(&result);

    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/response-cookie-capacity", port);
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_scheduler_request(
              scheduler, url, &request, 64 * 1024, 1000, &result)
          && result.set_cookie_count == FETCH_RESPONSE_COOKIE_CAPACITY
          && !result.set_cookies_truncated);
    fetch_result_destroy(&result);

    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/response-cookie-overflow", port);
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_scheduler_request(
              scheduler, url, &request, 64 * 1024, 1000, &result)
          && result.set_cookie_count == FETCH_RESPONSE_COOKIE_CAPACITY
          && result.set_cookies_truncated);
    fetch_result_destroy(&result);

    for (size_t i = 0;
         i < sizeof(rejected_paths) / sizeof(rejected_paths[0]); i++) {
        snprintf(url, sizeof(url), "http://127.0.0.1:%u/%s", port,
                 rejected_paths[i]);
        result = (FetchResult) {.budget = budget};
        /* The rule is a typed fact, not read back from the message. */
        CHECK(!fetch_scheduler_request(
              scheduler, url, &request, 64 * 1024, 1000, &result)
              && response_cookie_storage_empty(&result)
              && (strstr(result.error, "cookie") != NULL
                  || strstr(result.error, "Cookie") != NULL)
              && result.chain.failure == FETCH_CHAIN_FAILURE_RULE
              && result.chain.limit == FETCH_CHAIN_LIMIT_COOKIES);
        fetch_result_destroy(&result);
    }
    fetch_scheduler_destroy(scheduler);
    return true;
}

static bool test_response_cookie_allocation_failure(unsigned port)
{
    char url[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/response-cookie-capacity", port);
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true
    };
    bool observed_cookie_oom = false;
    for (size_t fail_after = 0;
         fail_after < 256 && !observed_cookie_oom; fail_after++) {
        Budget fault_budget;
        budget_init(&fault_budget, 2u * MIB);
        budget_inject_failure_after(&fault_budget, fail_after);
        FetchResult result = {.budget = &fault_budget};
        bool fetched = fetch_redirect(
            &fault_budget, url, &request, &result);
        observed_cookie_oom = !fetched
            && strstr(result.error,
                      "response Set-Cookie exceeded shared memory budget")
                   != NULL;
        /* Failure injection can also land on a best-effort allocation after
           all response cookies were committed (for example, shrinking the
           completed body buffer).  A successful request legitimately keeps
           its cookies; every failed request must still unwind them all. */
        CHECK(fetched
              ? result.set_cookie_count == FETCH_RESPONSE_COOKIE_CAPACITY
              : response_cookie_storage_empty(&result));
        budget_clear_failure_injection(&fault_budget);
        fetch_result_destroy(&result);
        CHECK(fault_budget.current == 0);
    }
    return observed_cookie_oom;
}

static bool test_informational_response_isolation(Budget *budget,
                                                  unsigned port)
{
    FetchRequest request = {
        .method = "GET", .cookie = "initial=kept",
        .allow_http_errors = true
    };
    char url[256];
    FetchResult result = {.budget = budget};
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/early-redirect", port);
    CHECK(fetch_redirect(budget, url, &request, &result)
          && result.status_code == 200
          && strcmp(result.etag, "\"final\"") == 0
          && result.accept_ch[0] == '\0'
          && result.critical_ch[0] == '\0'
          && strstr(result.response_headers, "x-early:") == NULL
          && result.set_cookie_count == 2
          && strcmp(result.set_cookies[0],
                    "hop=early-redirect; Path=/") == 0
          && strcmp(result.set_cookies[1],
                    "final_cookie=seen; Path=/") == 0
          && strstr(result.data, "early=must-not-survive") == NULL);
    fetch_result_destroy(&result);

    StreamProbe probe = {0};
    FetchStreamOptions stream = {
        .on_headers = stream_headers,
        .on_body = stream_body,
        .opaque = &probe
    };
    FetchStreamMetrics metrics = {0};
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/early-final", port);
    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64 * 1024);
    CHECK(scheduler != NULL);
    uint64_t id = fetch_scheduler_enqueue_stream(
        scheduler, url, &request, 64 * 1024, 5000, &stream);
    bool success = false, taken = false;
    result = (FetchResult) {.budget = budget};
    for (size_t pump = 0; id != 0 && pump < 2000 && !taken; pump++) {
        (void) fetch_scheduler_pump(scheduler, 1, 4);
        taken = fetch_scheduler_take_stream(
            scheduler, id, &success, &metrics, &result);
    }
    CHECK(id != 0 && taken && success
          && probe.header_calls == 1
          && metrics.headers_delivered
          && result.set_cookie_count == 1
          && strcmp(result.set_cookies[0],
                    "final_cookie=seen; Path=/") == 0
          && strstr(probe.body, "early=must-not-survive") == NULL);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    return true;
}

static bool test_duplicate_client_hint_headers(Budget *budget,
                                                unsigned port)
{
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true
    };
    char url[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/duplicate-client-hints", port);
    FetchResult result = {.budget = budget};
    CHECK(fetch_redirect(budget, url, &request, &result)
          && strcmp(result.accept_ch,
                    "Sec-CH-UA-Arch, Sec-CH-UA-Bitness") == 0
          && strcmp(result.critical_ch,
                    "Sec-CH-UA-Arch, Sec-CH-UA-Bitness") == 0);
    fetch_result_destroy(&result);

    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64 * 1024);
    CHECK(scheduler != NULL);
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_scheduler_request(
              scheduler, url, &request, 64 * 1024, 1000, &result)
          && strcmp(result.accept_ch,
                    "Sec-CH-UA-Arch, Sec-CH-UA-Bitness") == 0
          && strcmp(result.critical_ch,
                    "Sec-CH-UA-Arch, Sec-CH-UA-Bitness") == 0);
    fetch_result_destroy(&result);

    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/oversized-client-hints", port);
    CHECK(!fetch_scheduler_request(
              scheduler, url, &request, 64 * 1024, 1000, &result)
          && strstr(result.error, "Accept-CH") != NULL);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);

    result = (FetchResult) {.budget = budget};
    CHECK(!fetch_redirect(budget, url, &request, &result)
          && strstr(result.error, "Accept-CH") != NULL);
    fetch_result_destroy(&result);
    return true;
}

static bool test_proxy_connect_headers_suppressed(Budget *budget)
{
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true
    };
    StreamProbe probe = {0};
    FetchStreamOptions stream = {
        .on_headers = count_headers,
        .on_body = stream_body,
        .opaque = &probe
    };
    FetchStreamMetrics metrics = {0};
    FetchResult result = {.budget = budget};
    CHECK(!fetch_request_stream_cancelable(
              budget, "https://proxy-target.invalid/connect-test",
              &request, 64 * 1024, 1000, NULL, NULL, &stream, &metrics,
              &result)
          && probe.header_calls == 0
          && result.set_cookie_count == 0
          && result.accept_ch[0] == '\0'
          && result.critical_ch[0] == '\0'
          && strstr(result.response_headers, "x-proxy-only") == NULL);
    fetch_result_destroy(&result);

    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64 * 1024);
    CHECK(scheduler != NULL);
    memset(&probe, 0, sizeof(probe));
    uint64_t id = fetch_scheduler_enqueue_stream(
        scheduler, "https://proxy-target.invalid/connect-test",
        &request, 64 * 1024, 1000, &stream);
    bool success = true, taken = false;
    result = (FetchResult) {.budget = budget};
    for (size_t pump = 0; id != 0 && pump < 2000 && !taken; pump++) {
        (void) fetch_scheduler_pump(scheduler, 1, 4);
        taken = fetch_scheduler_take_stream(
            scheduler, id, &success, &metrics, &result);
    }
    CHECK(id != 0 && taken && !success
          && probe.header_calls == 0
          && result.set_cookie_count == 0
          && result.accept_ch[0] == '\0'
          && result.critical_ch[0] == '\0'
          && strstr(result.response_headers, "x-proxy-only") == NULL);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    return true;
}

/* One run of the sign-in handoff chain (run_fetch_redirect_test.py,
   account_chain): the POST and its redirects on the scheduler, with a
   cookie session, as a form submission makes them. */
static bool account_chain_run(Budget *budget, BrowserSession *browser,
                              unsigned port, const char *query,
                              FetchResult *result)
{
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/account-chain/pwd?%s",
             port, query);
    static const char body[] =
        "identifier=someone%40example.test&Passwd=synthetic&at=1";
    TilefinchRequestContext context = {
        .target_url = url, .initiator_url = url, .top_level_url = url,
        .method = "POST", .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_DOCUMENT,
        .top_level_navigation = true, .user_activated = true
    };
    FetchRequest request = {
        .method = "POST", .body = body, .body_length = sizeof(body) - 1u,
        .content_type = "application/x-www-form-urlencoded",
        .allow_http_errors = true,
        .credentials = FETCH_CREDENTIALS_INCLUDE,
        .credential_origin = url, .initiator_url = url,
        .cookie_session = browser, .cookie_context = &context
    };
    FetchScheduler *scheduler = fetch_scheduler_create(budget, 1, 64 * 1024);
    if (scheduler == NULL) return false;
    uint64_t id = fetch_scheduler_enqueue(scheduler, url, &request,
                                          64 * 1024, 5000);
    bool success = false, taken = false;
    *result = (FetchResult) {.budget = budget};
    for (size_t pump = 0; id != 0 && pump < 4000 && !taken; pump++) {
        (void) fetch_scheduler_pump(scheduler, 1, 4);
        taken = fetch_scheduler_take(scheduler, id, &success, result);
    }
    fetch_scheduler_destroy(scheduler);
    return id != 0 && taken && success;
}

/* Requests the host has seen for `step`, and (when asked) the length of
   the last token its setsid step received. */
static unsigned account_chain_hits_token(Budget *budget, unsigned port,
                                         const char *step, unsigned *token)
{
    char url[256];
    snprintf(url, sizeof(url),
             "http://localhost:%u/account-chain/hits?step=%s", port, step);
    FetchRequest request = {.method = "GET", .allow_http_errors = true};
    FetchResult result = {.budget = budget};
    unsigned hits = 9999u, seen = 0u;
    if (fetch_redirect(budget, url, &request, &result)
        && result.data != NULL)
        (void) sscanf(result.data, "hits=%u token=%u", &hits, &seen);
    fetch_result_destroy(&result);
    if (token != NULL) *token = seen;
    return hits;
}

static unsigned account_chain_hits(Budget *budget, unsigned port,
                                   const char *step)
{
    return account_chain_hits_token(budget, port, step, NULL);
}

/* The redirect chain after a password form (issue 7): a POST answered 302
   with the account's whole cookie set and several kilobytes of other
   headers, a cross-host hop whose Location carries a long one-time token,
   that host's own cookie set, and two more hops. The chain completes with
   every cookie, and its typed facts (FetchChainFacts) describe it; at each
   limit it ends with that limit's own reason, and a Location too long for a
   URL is refused, never followed cut short. */
static bool test_account_redirect_chain(Budget *budget, unsigned port,
                                        unsigned cross_port)
{
    BrowserSession browser;
    CHECK(browser_session_init(&browser, budget, 64 * 1024));
    BrowserCookieDiagnostics cookies = {0};
    browser_session_cookie_diagnostics(&browser, true, &cookies);
    FetchResult result;

    /* The larger end of a real handoff: 24 + 14 cookies, 4 KiB of other
       headers on the first two responses, a 1,200-byte token. */
    bool completed = account_chain_run(budget, &browser, port,
                                       "sidt=1200&pad=4096", &result);
    if (!completed)
        fprintf(stderr, "account chain: error=%s limit=%u hop=%u\n",
                result.error, (unsigned) result.chain.limit,
                (unsigned) result.chain.failed_hop);
    CHECK(completed && result.status_code == 200 && result.data != NULL
          /* The POST became a GET; the second host got its own 14 cookies
             back on the same chain, none of the first host's. */
          && strstr(result.data, "landed method=GET cookies=14 ") != NULL
          && strstr(result.data, " login-info=1") != NULL
          && result.set_cookie_count == 38u && !result.set_cookies_truncated
          && result.chain.redirects == 4u
          && result.chain.failure == FETCH_CHAIN_FAILURE_NONE
          && result.chain.limit == FETCH_CHAIN_LIMIT_NONE
          && result.chain.set_cookies == 38u
          && result.chain.maximum_header_bytes > 8000u
          && result.chain.first_byte_ms != 0u
          && result.chain.timeout_ms == 5000u);
    /* The jar takes the whole set with nothing refused or evicted. */
    for (size_t i = 0; i < result.set_cookie_count; i++)
        CHECK(browser_session_cookie_set_http(
            &browser, fetch_set_cookie_url(&result, i, ""),
            result.set_cookies[i]));
    char header[BROWSER_COOKIE_HEADER_CAPACITY];
    char root[64];
    snprintf(root, sizeof(root), "http://127.0.0.1:%u/", port);
    CHECK(cookies.refused == 0 && cookies.evictions == 0
          && browser_session_cookie_header(&browser, root, header,
                                           sizeof(header))
          && strstr(header, "LSID=") != NULL
          && strstr(header, "NID=") != NULL && strlen(header) > 3000u);
    fetch_result_destroy(&result);

    /* The transport keeps Set-Cookie fields over the whole chain, not per
       response: a chain that passes 64 only across its hops (84 here, none
       more than 24 on one response) completes with all of them. */
    completed = account_chain_run(budget, &browser, port,
                                  "sidt=600&wide=1", &result);
    if (!completed)
        fprintf(stderr, "wide account chain: error=%s limit=%u hop=%u\n",
                result.error, (unsigned) result.chain.limit,
                (unsigned) result.chain.failed_hop);
    CHECK(completed && result.data != NULL
          && strstr(result.data, "landed method=GET cookies=14 ") != NULL
          && result.set_cookie_count == 84u && !result.set_cookies_truncated
          && result.chain.redirects == 4u
          && result.chain.set_cookies == 84u
          && result.chain.hop_set_cookies == 8u);
    fetch_result_destroy(&result);

    /* A navigation's chain passes through a Location far longer than a
       document's address may be (a sign-in hand-over's one-time token runs
       to several kilobytes): the hop is made with the whole address, the
       second host's cookies come back, and the chain completes. The
       cross-host address is
       "http://localhost:PORT/account-chain/setsid?ssdc=1&sidt=" plus the
       token. */
    char prefix[96];
    int prefix_length = snprintf(
        prefix, sizeof(prefix),
        "http://localhost:%u/account-chain/setsid?ssdc=1&sidt=", cross_port);
    CHECK(prefix_length > 0);
    char query[64];
    unsigned before = account_chain_hits(budget, cross_port, "setsid");
    static const unsigned location_lengths[] = {
        TILEFINCH_URL_SERIALIZED_LIMIT - 1u, TILEFINCH_URL_SERIALIZED_LIMIT,
        3000u, 7000u, TILEFINCH_URL_LONG_LIMIT - 1u
    };
    for (size_t at = 0;
         at < sizeof(location_lengths) / sizeof(location_lengths[0]); at++) {
        unsigned token = location_lengths[at] - (unsigned) prefix_length;
        unsigned seen = 0;
        snprintf(query, sizeof(query), "sidt=%u&pad=4096", token);
        completed = account_chain_run(budget, &browser, port, query, &result);
        if (!completed)
            fprintf(stderr, "account chain, %u-byte Location: error=%s "
                            "limit=%u hop=%u\n", location_lengths[at],
                    result.error, (unsigned) result.chain.limit,
                    (unsigned) result.chain.failed_hop);
        CHECK(completed && result.status_code == 200 && result.data != NULL
              && strstr(result.data, "landed method=GET cookies=14 ") != NULL
              && strstr(result.data, " login-info=1") != NULL
              && result.set_cookie_count == 38u
              && !result.set_cookies_truncated
              && result.chain.redirects == 4u
              && result.chain.limit == FETCH_CHAIN_LIMIT_NONE
              /* The response's address is the landing page's. */
              && strstr(result.effective_url, "/account-chain/landed") != NULL
              && account_chain_hits_token(budget, cross_port, "setsid",
                                          &seen) == before + 1u + at
              /* Whole: not one byte of the token was cut. */
              && seen == token);
        /* The long hop's cookies are attributed to its own address. */
        bool long_hop_cookie = false;
        for (size_t i = 0; i < result.set_cookie_count; i++) {
            const char *source = fetch_set_cookie_url(&result, i, "");
            if (strncmp(source, prefix, (size_t) prefix_length) == 0
                && strlen(source) == location_lengths[at])
                long_hop_cookie = true;
        }
        CHECK(long_hop_cookie);
        fetch_result_destroy(&result);
    }
    before += sizeof(location_lengths) / sizeof(location_lengths[0]);

    /* One byte past a hop's bound is refused: the hop is not made with the
       address cut short (the second host never hears of it), and the
       failure names the rule and the hop, not a timeout. */
    snprintf(query, sizeof(query), "sidt=%u",
             (unsigned) (TILEFINCH_URL_LONG_LIMIT - prefix_length));
    CHECK(!account_chain_run(budget, &browser, port, query, &result)
          && !result.timed_out
          && result.chain.failure == FETCH_CHAIN_FAILURE_RULE
          && result.chain.limit == FETCH_CHAIN_LIMIT_TARGET_LENGTH
          && result.chain.redirects == 1u && result.chain.failed_hop == 1u
          && strcmp(result.chain.failed_host, "127.0.0.1") == 0
          && account_chain_hits(budget, cross_port, "setsid") == before);
    fetch_result_destroy(&result);

    /* Other headers past the transport's 8 KiB snapshot leave no room in
       it for the Location. It is kept beside the snapshot, so the chain
       still completes. */
    completed = account_chain_run(budget, &browser, port,
                                  "sidt=3000&pad=9000", &result);
    if (!completed)
        fprintf(stderr, "account chain header overflow: error=%s failure=%u "
                        "limit=%u redirects=%u hop=%u\n", result.error,
                (unsigned) result.chain.failure,
                (unsigned) result.chain.limit,
                (unsigned) result.chain.redirects,
                (unsigned) result.chain.failed_hop);
    CHECK(completed && result.data != NULL
          && strstr(result.data, "landed method=GET cookies=14 ") != NULL
          && result.chain.redirects == 4u
          && result.chain.maximum_header_bytes > 8192u
          && account_chain_hits(budget, cross_port, "setsid")
                 == before + 1u);
    fetch_result_destroy(&result);
    before++;

    /* A chain may pass through a long address, not end on one: nothing
       past the transport holds a document address over the smaller bound.
       The hop is made (the server answers it) and the chain then fails
       under its own rule, with no response handed on. */
    CHECK(!account_chain_run(budget, &browser, port, "sidt=3000&end=1",
                             &result)
          && !result.timed_out
          && result.chain.failure == FETCH_CHAIN_FAILURE_RULE
          && result.chain.limit == FETCH_CHAIN_LIMIT_DOCUMENT_LENGTH
          && result.chain.redirects == 2u && result.chain.failed_hop == 2u
          && strcmp(result.chain.failed_host, "localhost") == 0
          && account_chain_hits(budget, cross_port, "setsid")
                 == before + 1u);
    fetch_result_destroy(&result);
    before++;
    /* The same chain with an address a document may have ends well. */
    CHECK(account_chain_run(budget, &browser, port, "sidt=600&end=1",
                            &result)
          && result.status_code == 200 && result.data != NULL
          && strcmp(result.data, "ended on the token address") == 0
          && result.chain.redirects == 2u);
    fetch_result_destroy(&result);
    before++;

    /* Only a navigation's chain: any other request's hop keeps the bound
       of a stored address, and a longer Location is refused before the
       hop is made. */
    char check[160];
    FetchRequest plain = {.method = "GET", .allow_http_errors = true};
    FetchScheduler *plain_scheduler =
        fetch_scheduler_create(budget, 1, 64 * 1024);
    CHECK(plain_scheduler != NULL);
    snprintf(check, sizeof(check),
             "http://127.0.0.1:%u/account-chain/check?sidt=%u", port,
             (unsigned) (TILEFINCH_URL_SERIALIZED_LIMIT - prefix_length));
    result = (FetchResult) {.budget = budget};
    CHECK(!fetch_scheduler_request(plain_scheduler, check, &plain,
                                   64 * 1024, 5000, &result)
          && result.chain.failure == FETCH_CHAIN_FAILURE_RULE
          && result.chain.limit == FETCH_CHAIN_LIMIT_TARGET_LENGTH
          && result.chain.redirects == 0u
          && account_chain_hits(budget, cross_port, "setsid") == before);
    fetch_result_destroy(&result);
    snprintf(check, sizeof(check),
             "http://127.0.0.1:%u/account-chain/check?sidt=%u", port,
             (unsigned) (TILEFINCH_URL_SERIALIZED_LIMIT - 1 - prefix_length));
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_scheduler_request(plain_scheduler, check, &plain,
                                  64 * 1024, 5000, &result)
          && result.chain.redirects == 3u
          && account_chain_hits(budget, cross_port, "setsid")
                 == before + 1u);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(plain_scheduler);

    /* A request whose redirect mode is "error" names that rule too. */
    char url[128];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/account-chain/handoff", port);
    FetchRequest forbidden = {
        .method = "GET", .allow_http_errors = true,
        .page_redirect_mode = FETCH_PAGE_REDIRECT_ERROR
    };
    FetchScheduler *scheduler = fetch_scheduler_create(budget, 1, 64 * 1024);
    CHECK(scheduler != NULL);
    result = (FetchResult) {.budget = budget};
    CHECK(!fetch_scheduler_request(scheduler, url, &forbidden, 64 * 1024,
                                   5000, &result)
          && strstr(result.error, "redirect mode forbids") != NULL
          && result.chain.failure == FETCH_CHAIN_FAILURE_RULE
          && result.chain.limit == FETCH_CHAIN_LIMIT_TARGET
          && result.chain.failed_hop == 0u);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    browser_session_destroy(&browser);
    return true;
}
