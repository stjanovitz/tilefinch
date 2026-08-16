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

    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/response-cookie-capacity", port);
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_redirect(budget, url, &request, &result)
          && result.set_cookie_count == FETCH_RESPONSE_COOKIE_CAPACITY);
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

    static const char *rejected_paths[] = {
        "oversized-cookie", "response-cookie-overflow"
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
          && result.set_cookie_count == FETCH_RESPONSE_COOKIE_CAPACITY);
    fetch_result_destroy(&result);

    for (size_t i = 0;
         i < sizeof(rejected_paths) / sizeof(rejected_paths[0]); i++) {
        snprintf(url, sizeof(url), "http://127.0.0.1:%u/%s", port,
                 rejected_paths[i]);
        result = (FetchResult) {.budget = budget};
        CHECK(!fetch_scheduler_request(
              scheduler, url, &request, 64 * 1024, 1000, &result)
              && response_cookie_storage_empty(&result)
              && (strstr(result.error, "cookie") != NULL
                  || strstr(result.error, "Cookie") != NULL));
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

