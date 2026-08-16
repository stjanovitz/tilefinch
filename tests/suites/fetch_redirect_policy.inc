static bool test_redirect_callback_abort_classification(void)
{
    CHECK(!tilefinch_fetch_redirect_callback_abort_expected(
              CURLE_OK, true, true, false));
    CHECK(tilefinch_fetch_redirect_callback_abort_expected(
              CURLE_WRITE_ERROR, true, true, false));
    CHECK(tilefinch_fetch_redirect_callback_abort_expected(
              CURLE_HTTP2, true, true, false));
    CHECK(tilefinch_fetch_redirect_callback_abort_expected(
              CURLE_ABORTED_BY_CALLBACK, true, true, false));
    CHECK(!tilefinch_fetch_redirect_callback_abort_expected(
              CURLE_RECV_ERROR, false, true, false));
    CHECK(!tilefinch_fetch_redirect_callback_abort_expected(
              CURLE_RECV_ERROR, true, false, false));
    CHECK(!tilefinch_fetch_redirect_callback_abort_expected(
              CURLE_RECV_ERROR, true, true, true));
    return true;
}

static bool test_private_network_admission(Budget *budget, unsigned port)
{
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/final", port);
    TilefinchRequestContext context = {
        .target_url = url,
        .initiator_url = "http://public.example/page",
        .top_level_url = "http://public.example/page",
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_IMAGE
    };
    FetchRequest transport = {0};
    FetchPreparedPageRequest prepared;
    const FetchRequest *request = NULL;
    FetchResult result = {.budget = budget};

    /* PNA classifies carrier-grade NAT and benchmark networks as non-public.
       The resolved-peer hook must reject both native and mapped-IPv6 forms
       before attempting a connection. */
    static const char *const special_targets[] = {
        "http://100.64.0.1/",
        "http://198.18.0.1/",
        "http://[::ffff:100.64.0.1]/",
        "http://[::ffff:198.18.0.1]/"
    };
    for (size_t i = 0;
        i < sizeof(special_targets) / sizeof(special_targets[0]); i++) {
        context.target_url = special_targets[i];
        CHECK(fetch_prepare_page_request_context(
            &context, context.initiator_url, NULL, NULL, NULL, NULL,
            &transport, &prepared, NULL));
        request = fetch_prepared_page_request(&prepared);
        result = (FetchResult) {.budget = budget};
        CHECK(!fetch_redirect(
                  budget, special_targets[i], request, &result)
              && strstr(result.error, "private network") != NULL);
        fetch_result_destroy(&result);
    }
    context.target_url = url;
    CHECK(fetch_prepare_page_request_context(
        &context, context.initiator_url, NULL, NULL, NULL, NULL,
        &transport, &prepared, NULL));
    request = fetch_prepared_page_request(&prepared);
    result = (FetchResult) {.budget = budget};
    CHECK(!fetch_redirect(budget, url, request, &result)
          && strstr(result.error, "private network") != NULL);
    fetch_result_destroy(&result);
    result = (FetchResult) {.budget = budget};
    bool scheduled_blocked = fetch_scheduled(
        budget, url, request, &result);
    if (scheduled_blocked || strstr(result.error, "private network") == NULL) {
        fprintf(stderr,
                "scheduled PNA diagnostic: success=%u status=%ld error=%s\n",
                scheduled_blocked ? 1u : 0u, result.status_code,
                result.error);
    }
    CHECK(!scheduled_blocked
          && strstr(result.error, "private network") != NULL);
    fetch_result_destroy(&result);

    /* An explicitly entered local top-level navigation is the narrow user
       escape hatch. The public-page case above remains protected even when
       its destination is a top-level-looking URL. */
    context.initiator_url = NULL;
    context.top_level_url = url;
    context.mode = TILEFINCH_REQUEST_MODE_NAVIGATE;
    context.destination = TILEFINCH_DESTINATION_DOCUMENT;
    context.top_level_navigation = true;
    context.user_activated = true;
    CHECK(fetch_prepare_page_request_context(
        &context, NULL, NULL, NULL, NULL, NULL,
        &transport, &prepared, NULL));
    request = fetch_prepared_page_request(&prepared);
    result = (FetchResult) {.budget = budget};
    bool entered_local = fetch_redirect(budget, url, request, &result);
    if (!entered_local || result.status_code != 200) {
        fprintf(stderr,
                "entered LAN navigation diagnostic: success=%u status=%ld error=%s\n",
                entered_local ? 1u : 0u, result.status_code, result.error);
    }
    CHECK(entered_local && result.status_code == 200);
    fetch_result_destroy(&result);

    /* Once loaded, a local page may continue using its own address space.
       This positive control traverses the same resolved-peer hook. */
    context.initiator_url = url;
    context.top_level_url = url;
    context.mode = TILEFINCH_REQUEST_MODE_NO_CORS;
    context.destination = TILEFINCH_DESTINATION_IMAGE;
    context.top_level_navigation = false;
    context.user_activated = false;
    CHECK(fetch_prepare_page_request_context(
        &context, url, NULL, NULL, NULL, NULL,
        &transport, &prepared, NULL));
    request = fetch_prepared_page_request(&prepared);
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_redirect(budget, url, request, &result)
          && result.status_code == 200);
    fetch_result_destroy(&result);
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_scheduled(budget, url, request, &result)
          && result.status_code == 200);
    fetch_result_destroy(&result);
    return true;
}

static bool test_hsts_is_security_header(Budget *budget, unsigned port)
{
    char url[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/hsts-filled-final", port);
    FetchRequest request = {.method = "GET", .allow_http_errors = true};
    for (size_t mode = 0; mode < 2; mode++) {
        FetchResult result = {.budget = budget};
        bool fetched = mode == 0
            ? fetch_redirect(budget, url, &request, &result)
            : fetch_scheduled(budget, url, &request, &result);
        CHECK(fetched && result.status_code == 200
              && result.security.version
                     == FETCH_RESPONSE_SECURITY_METADATA_VERSION
              && result.security.hsts_state == FETCH_SECURITY_FIELD_VALID
              && result.security.hsts_max_age == 3600
              && strstr(result.response_headers,
                        "strict-transport-security:") == NULL);
        fetch_result_destroy(&result);
    }
    return true;
}

static bool hsts_redirect_validator_saw_upgrade;

static bool hsts_redirect_validator(const char *url)
{
    if (url != NULL && strncmp(url, "https://hsts.test:", 18) == 0) {
        hsts_redirect_validator_saw_upgrade = true;
    }
    return true;
}

static bool test_hsts_applies_to_scheduled_redirects(Budget *budget,
                                                     unsigned port)
{
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/hsts-redirect", port);
    BrowserSession session = {0};
    CHECK(browser_session_init(&session, budget, 64 * 1024));
    static const char hsts[] =
        "strict-transport-security: max-age=3600\n";
    CHECK(browser_session_hsts_observe(
        &session, "https://hsts.test/", hsts, sizeof(hsts) - 1u));
    TilefinchRequestContext context = {
        .target_url = url,
        .initiator_url = url,
        .top_level_url = url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_IMAGE,
        .top_level_navigation = false,
        .user_activated = false
    };
    FetchRequest request = {
        .method = "GET",
        .allow_http_errors = true,
        .redirect_url_validator = hsts_redirect_validator,
        .cookie_session = &session,
        .cookie_context = &context,
        .credentials = FETCH_CREDENTIALS_INCLUDE
    };
    CHECK(fetch_request_validate(&request, NULL));

    /* The upgraded redirect intentionally reaches an HTTP-only loopback
       fixture over TLS, so transport failure is expected. The policy hook
       observes the URL after HSTS and before the connection attempt. */
    FetchResult result = {.budget = budget};
    hsts_redirect_validator_saw_upgrade = false;
    (void) fetch_redirect(budget, url, &request, &result);
    CHECK(hsts_redirect_validator_saw_upgrade);
    fetch_result_destroy(&result);

    result = (FetchResult) {.budget = budget};
    hsts_redirect_validator_saw_upgrade = false;
    (void) fetch_scheduled(budget, url, &request, &result);
    CHECK(hsts_redirect_validator_saw_upgrade);
    fetch_result_destroy(&result);
    browser_session_destroy(&session);
    return true;
}

static bool test_stylesheet_redirect_rechecks_csp(Budget *budget,
                                                  unsigned port)
{
    char document_url[256], html[512];
    snprintf(document_url, sizeof(document_url),
             "http://127.0.0.1:%u/page", port);
    snprintf(html, sizeof(html),
             "<!doctype html><link rel=stylesheet "
             "href='http://127.0.0.1:%u/stylesheet-cross'>"
             "<p id=csp-probe>blocked redirect</p>", port);
    BrowserSession session = {0};
    PocDocument document = {0};
    Stylesheet stylesheet = {0};
    StylesheetDocumentResources resources = {0};
    ExternalStylesheetStats stats = {0};
    bool session_ready = browser_session_init(&session, budget, 64 * 1024);
    bool document_ready = session_ready
        && document_parse(&document, budget, html, strlen(html), strlen(html));
    static const char policy[] =
        "content-security-policy: style-src 'self'\n";
    bool ready = document_ready
        && tilefinch_csp_parse_response_headers(
               &document.content_security_policy, document_url,
               policy, sizeof(policy) - 1u, false)
        && stylesheet_build(&stylesheet, budget, &document, 480);
    bool loaded = ready && stylesheets_load_external_tracked_with_context(
        &document, &stylesheet, budget, document_url, document_url, "",
        4, 4096, 4096, 3000, NULL, &session, &resources, &stats);
    CHECK(loaded && stats.discovered == 1 && stats.attempted == 1
          && stats.loaded == 0 && stats.failed == 1);
    stylesheet_document_resources_destroy(&resources);
    stylesheet_destroy(&stylesheet);
    document_destroy(&document);
    browser_session_destroy(&session);
    return true;
}

static bool test_redirect_callback_abort_shape(Budget *budget,
                                               unsigned port)
{
    char url[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/callback-abort-shape", port);
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true
    };
    FetchResult result = {.budget = budget};
    CHECK(fetch_redirect(budget, url, &request, &result)
          && result.status_code == 200
          && strstr(result.data, "query=callback=followed") != NULL
          && strstr(result.data, "redirect-body-must-not-appear") == NULL
          && strstr(result.response_headers,
                    "x-hallmonitor-challenge:") == NULL
          && result.set_cookie_count == 3);
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
          && result.status_code == 200
          && strstr(result.data, "query=callback=followed") != NULL
          && strstr(result.data, "redirect-body-must-not-appear") == NULL
          && result.set_cookie_count == 3);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    return true;
}

static bool test_supported_accept_encoding(Budget *budget, unsigned port)
{
    const curl_version_info_data *version =
        curl_version_info(CURLVERSION_NOW);
    long features = version == NULL ? 0 : version->features;
    bool deflate = (features & CURL_VERSION_LIBZ) != 0;
    bool brotli = false;
#if defined(CURL_VERSION_BROTLI)
    brotli = (features & CURL_VERSION_BROTLI) != 0;
#endif
    const char *encoding = deflate
        ? (brotli ? "gzip, deflate, br" : "gzip, deflate")
        : (brotli ? "br" : "identity");
    char expected[64];
    int written = snprintf(expected, sizeof(expected),
                           "encoding=%s", encoding);
    CHECK(written > 0 && (size_t) written < sizeof(expected));

    CHECK(setenv("TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI", "1", 1) == 0);
    char url[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/accept-encoding", port);
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true
    };
    FetchResult result = {.budget = budget};
    CHECK(fetch_redirect(budget, url, &request, &result)
          && result.status_code == 200
          && strcmp(result.data, expected) == 0);
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
          && result.status_code == 200
          && strcmp(result.data, expected) == 0);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    CHECK(unsetenv("TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI") == 0);
    return true;
}

static bool test_same_host_redirect(Budget *budget, unsigned port)
{
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/same", port);
    BrowserSession browser;
    CHECK(browser_session_init(&browser, budget, 64 * 1024)
          && browser_session_cookie_set_http(
              &browser, url, "secret=initial; Path=/"));
    TilefinchRequestContext cookie_context = {
        .target_url = url, .initiator_url = url, .top_level_url = url,
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_DOCUMENT,
        .top_level_navigation = true
    };
    FetchRequest request = {
        .method = "GET", .cookie = "secret=initial",
        .origin = "http://origin.example",
        .allow_http_errors = true,
        .cookie_session = &browser,
        .cookie_context = &cookie_context
    };
    FetchResult result = {.budget = budget};
    CHECK(fetch_redirect(budget, url, &request, &result));
    CHECK(result.status_code == 200
          && strcmp(result.etag, "\"final\"") == 0
          && result.critical_ch[0] == '\0'
          && strstr(result.response_headers, "x-hop: final\n") != NULL
          && strstr(result.response_headers, "x-intermediate:") == NULL
          && strstr(result.data, "method=GET") != NULL
          && strstr(result.data, "secret=initial") != NULL
          && strstr(result.data, "hop=seen") != NULL
          && strstr(result.data, "origin=http://origin.example") != NULL
          && strstr(result.data, "redirect-body-must-not-appear") == NULL
          && result.set_cookie_count == 2
          && strstr(fetch_set_cookie_url(&result, 0, ""), "/same") != NULL
          && strstr(fetch_set_cookie_url(&result, 1, ""), "/final") != NULL);
    fetch_result_destroy(&result);

    StreamProbe probe = {0};
    FetchStreamOptions stream = {
        .on_headers = stream_headers,
        .on_body = stream_body,
        .opaque = &probe
    };
    FetchStreamMetrics metrics;
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_request_stream_cancelable(
              budget, url, &request, 64 * 1024, 5000, NULL, NULL, &stream,
              &metrics, &result)
          && probe.header_calls == 1
          && metrics.headers_delivered
          && strstr(probe.body, "method=GET") != NULL
          && strstr(probe.body, "redirect-body-must-not-appear") == NULL);
    fetch_result_destroy(&result);
    browser_session_destroy(&browser);
    return true;
}

static bool redirect_only_same_path(const char *url)
{
    return url != NULL && strstr(url, "/same") != NULL;
}

static bool test_redirect_url_validator(Budget *budget, unsigned port)
{
    size_t baseline = budget->current;
    char url[256];
    int written = snprintf(
        url, sizeof(url), "http://127.0.0.1:%u/same", port);
    CHECK(written > 0 && (size_t) written < sizeof(url));
    FetchRequest request = {
        .method = "GET",
        .allow_http_errors = true,
        .credentials = FETCH_CREDENTIALS_OMIT,
        .redirect_url_validator = redirect_only_same_path
    };
    FetchResult result = {.budget = budget};
    CHECK(!fetch_redirect(budget, url, &request, &result));
    fetch_result_destroy(&result);

    result = (FetchResult) {.budget = budget};
    CHECK(!fetch_scheduled(budget, url, &request, &result));
    fetch_result_destroy(&result);

    written = snprintf(
        url, sizeof(url), "http://127.0.0.1:%u/final", port);
    CHECK(written > 0 && (size_t) written < sizeof(url));
    result = (FetchResult) {.budget = budget};
    CHECK(!fetch_redirect(budget, url, &request, &result));
    fetch_result_destroy(&result);
    return budget->current == baseline;
}

static bool test_single_hop_redirect_boundary(Budget *budget, unsigned port)
{
    size_t baseline = budget->current;
    char url[256];
    int written = snprintf(
        url, sizeof(url), "http://127.0.0.1:%u/same", port);
    CHECK(written > 0 && (size_t) written < sizeof(url));
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true,
        .credentials = FETCH_CREDENTIALS_OMIT
    };
    FetchResult result = {.budget = budget};
    CHECK(fetch_request_single_hop(
              budget, url, &request, 64u * 1024u, 5000, &result)
          && result.status_code == 302
          && strcmp(result.effective_url, url) == 0
          && strcmp(result.etag, "\"intermediate\"") == 0
          && strstr(result.response_headers, "location: /final\n") != NULL
          && strstr(result.response_headers,
                    "x-intermediate: must-not-survive\n") != NULL
          && strstr(result.data, "redirect-body-must-not-appear") != NULL
          && result.set_cookie_count == 0);
    fetch_result_destroy(&result);

    written = snprintf(
        url, sizeof(url), "http://127.0.0.1:%u/head", port);
    CHECK(written > 0 && (size_t) written < sizeof(url));
    request.method = "HEAD";
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_request_single_hop(
              budget, url, &request, 64u * 1024u, 5000, &result)
          && result.status_code == 200
          && strcmp(result.effective_url, url) == 0
          && result.length == 0 && result.data != NULL
          && result.data[0] == '\0');
    fetch_result_destroy(&result);
    return budget->current == baseline;
}

static bool test_logical_url_fragments(Budget *budget, unsigned port)
{
    size_t baseline = budget->current;
    static const struct {
        const char *request_path;
        const char *effective_path;
        const char *query_marker;
    } cases[] = {
        {"/final#client", "/final#client", "query="},
        {"/fragment-inherit#client",
         "/final?fragment=inherit#client", "query=fragment=inherit"},
        {"/fragment-replace#client",
         "/final?fragment=replace#server", "query=fragment=replace"},
        {"/fragment-empty#client",
         "/final?fragment=empty#", "query=fragment=empty"},
        {"/fragment-chain-start#client",
         "/final?fragment=chain#middle", "query=fragment=chain"},
    };
    FetchRequest request = {.method = "GET", .allow_http_errors = true};
    for (size_t mode = 0; mode < 2; mode++) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            char url[256], expected[256];
            int url_written = snprintf(
                url, sizeof(url), "http://127.0.0.1:%u%s",
                port, cases[i].request_path);
            int expected_written = snprintf(
                expected, sizeof(expected), "http://127.0.0.1:%u%s",
                port, cases[i].effective_path);
            CHECK(url_written > 0 && (size_t) url_written < sizeof(url)
                  && expected_written > 0
                  && (size_t) expected_written < sizeof(expected));
            FetchResult result = {.budget = budget};
            bool fetched = mode == 0
                ? fetch_redirect(budget, url, &request, &result)
                : fetch_scheduled(budget, url, &request, &result);
            CHECK(fetched && result.status_code == 200
                  && strcmp(result.effective_url, expected) == 0
                  && strstr(result.data, cases[i].query_marker) != NULL
                  && strstr(result.data, "wire-fragment=0") != NULL
                  && response_cookie_urls_are_fragmentless(&result));
            fetch_result_destroy(&result);
        }
    }
    return budget->current == baseline;
}

static bool test_redirect_referrer_policy(Budget *budget, unsigned port)
{
    size_t baseline = budget->current;
    char source[256], initial_referer[256], url[256], expected_origin[256];
    int source_written = snprintf(
        source, sizeof(source),
        "http://127.0.0.1:%u/source/path?private=1#source", port);
    int referer_written = snprintf(
        initial_referer, sizeof(initial_referer),
        "http://127.0.0.1:%u/source/path?private=1", port);
    int expected_written = snprintf(
        expected_origin, sizeof(expected_origin),
        "referer=http://127.0.0.1:%u/", port);
    CHECK(source_written > 0 && (size_t) source_written < sizeof(source)
          && referer_written > 0
          && (size_t) referer_written < sizeof(initial_referer)
          && expected_written > 0
          && (size_t) expected_written < sizeof(expected_origin));

    FetchRequest request = {
        .method = "GET", .allow_http_errors = true,
        .referer = initial_referer, .referrer_source = source,
        .referrer_policy = "no-referrer"
    };
    for (size_t mode = 0; mode < 2; mode++) {
        snprintf(url, sizeof(url),
                 "http://127.0.0.1:%u/referrer-policy-start#logical", port);
        FetchResult result = {.budget = budget};
        bool fetched = mode == 0
            ? fetch_redirect(budget, url, &request, &result)
            : fetch_scheduled(budget, url, &request, &result);
        CHECK(fetched && result.status_code == 200
              && strstr(result.data, "query=policy_middle=1") != NULL
              && strstr(result.data, expected_origin) != NULL
              && strstr(result.effective_url,
                        "/final?policy_middle=1#logical") != NULL);
        fetch_result_destroy(&result);

        snprintf(url, sizeof(url),
                 "http://127.0.0.1:%u/referrer-policy-invalid", port);
        request.referer = NULL;
        result = (FetchResult) {.budget = budget};
        fetched = mode == 0
            ? fetch_redirect(budget, url, &request, &result)
            : fetch_scheduled(budget, url, &request, &result);
        CHECK(fetched && result.status_code == 200
              && strstr(result.data, "query=policy_invalid=1") != NULL
              && strstr(result.data, "referer=<none>") != NULL);
        fetch_result_destroy(&result);
        request.referer = initial_referer;
    }
    return budget->current == baseline;
}

static bool test_referrer_policy_metadata_helper(void)
{
    FetchResult result = {0};
    static const char headers[] =
        "content-type: text/plain\n"
        "referrer-policy: invalid-future\n"
        "Referrer-Policy: same-origin, ORIGIN, another-future-token\n";
    memcpy(result.response_headers, headers, sizeof(headers) - 1);
    result.response_headers_length = sizeof(headers) - 1;

    bool present = false;
    char policy[FETCH_REFERRER_POLICY_LIMIT];
    CHECK(fetch_response_referrer_policy(&result, &present, policy)
          && present && strcmp(policy, "origin") == 0);

    static const char invalid_headers[] =
        "referrer-policy: invalid-one, invalid-two\n";
    memcpy(result.response_headers, invalid_headers,
           sizeof(invalid_headers) - 1);
    result.response_headers_length = sizeof(invalid_headers) - 1;
    CHECK(fetch_response_referrer_policy(&result, &present, policy)
          && present && policy[0] == '\0');

    static const char absent_headers[] = "content-type: text/plain\n";
    memcpy(result.response_headers, absent_headers,
           sizeof(absent_headers) - 1);
    result.response_headers_length = sizeof(absent_headers) - 1;
    CHECK(fetch_response_referrer_policy(&result, &present, policy)
          && !present && policy[0] == '\0');
    return true;
}

static bool test_referrer_policy_beyond_header_snapshot(
    Budget *budget, unsigned port)
{
    size_t baseline = budget->current;
    char source[256], initial_referer[256], url[256];
    CHECK(snprintf(
              source, sizeof(source),
              "http://127.0.0.1:%u/source/private#source", port) > 0
          && snprintf(
                 initial_referer, sizeof(initial_referer),
                 "http://127.0.0.1:%u/source/private", port) > 0);
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true,
        .referer = initial_referer, .referrer_source = source,
        .referrer_policy = "unsafe-url"
    };
    for (size_t mode = 0; mode < 2; mode++) {
        snprintf(url, sizeof(url),
                 "http://127.0.0.1:%u/referrer-policy-filled", port);
        FetchResult result = {.budget = budget};
        bool fetched = mode == 0
            ? fetch_redirect(budget, url, &request, &result)
            : fetch_scheduled(budget, url, &request, &result);
        CHECK(fetched && result.status_code == 200
              && strstr(result.data, "query=policy_filled=1") != NULL
              && strstr(result.data, "referer=<none>") != NULL);
        fetch_result_destroy(&result);

        snprintf(url, sizeof(url),
                 "http://127.0.0.1:%u/referrer-policy-filled-final", port);
        result = (FetchResult) {.budget = budget};
        fetched = mode == 0
            ? fetch_redirect(budget, url, &request, &result)
            : fetch_scheduled(budget, url, &request, &result);
        bool present = false;
        char policy[FETCH_REFERRER_POLICY_LIMIT];
        CHECK(fetched && result.status_code == 200
              && result.response_referrer_policy_metadata_valid
              && fetch_response_referrer_policy(
                     &result, &present, policy)
              && present && strcmp(policy, "no-referrer") == 0
              && strstr(result.response_headers,
                        "referrer-policy:") == NULL);
        fetch_result_destroy(&result);
    }
    PolicyStreamProbe probe = {0};
    FetchStreamOptions stream = {
        .on_headers = policy_stream_headers,
        .on_body = policy_stream_body,
        .opaque = &probe
    };
    FetchStreamMetrics metrics = {0};
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/referrer-policy-filled-final", port);
    FetchResult streamed = {.budget = budget};
    CHECK(fetch_request_stream_cancelable(
              budget, url, &request, 64 * 1024, 5000, NULL, NULL,
              &stream, &metrics, &streamed)
          && probe.header_calls == 1 && probe.policy_ok
          && probe.body_bytes == strlen("filled-policy-final"));
    fetch_result_destroy(&streamed);
    return budget->current == baseline;
}

static bool test_empty_location_reference(Budget *budget, unsigned port)
{
    size_t baseline = budget->current;
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true
    };
    for (size_t mode = 0; mode < 2; mode++) {
        char url[256], marker[64];
        snprintf(url, sizeof(url),
                 "http://127.0.0.1:%u/location-empty?mode=%zu#client",
                 port, mode);
        snprintf(marker, sizeof(marker), "query=mode=%zu", mode);
        FetchResult result = {.budget = budget};
        bool fetched = mode == 0
            ? fetch_redirect(budget, url, &request, &result)
            : fetch_scheduled(budget, url, &request, &result);
        CHECK(fetched && result.status_code == 200
              && strcmp(result.effective_url, url) == 0
              && strstr(result.data, marker) != NULL
              && strstr(result.data, "wire-fragment=0") != NULL);
        fetch_result_destroy(&result);
    }
    return budget->current == baseline;
}

static bool test_scheduled_buffered_body_is_cooperative(Budget *budget,
                                                        unsigned port)
{
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/large", port);
    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64u * 1024u);
    CHECK(scheduler != NULL);
    FetchRequest request = {.method = "GET", .allow_http_errors = true};
    uint64_t id = fetch_scheduler_enqueue(
        scheduler, url, &request, 64u * 1024u, 5000);
    CHECK(id != 0);
    FetchPumpQuota quota = {
        .maximum_body_callbacks = 1,
        .maximum_body_bytes = 1024,
        .maximum_time_us = 100000
    };
    FetchResult result = {.budget = budget};
    bool success = false, taken = false, bounded = true, yielded = false;
    size_t pump_count = 0;
    for (; pump_count < 512 && !taken; pump_count++) {
        FetchPumpMetrics metrics;
        (void) fetch_scheduler_pump_bounded(
            scheduler, 1, 4, &quota, &metrics);
        bounded = bounded && metrics.body_callbacks <= 1
                  && metrics.body_bytes <= 1024;
        yielded = yielded || metrics.quota_yielded;
        taken = fetch_scheduler_take(scheduler, id, &success, &result);
    }
    CHECK(bounded && yielded && pump_count > 1 && taken && success
          && result.length == 48u * 1024u && result.data != NULL);
    for (size_t i = 0; i < result.length; i++) {
        CHECK((unsigned char) result.data[i]
              == (unsigned char) ((i * 37u + 11u) & 0xffu));
    }
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    return true;
}

/* Browser navigation, parser-discovered resources, and page fetch() calls
   own separate schedulers but share libcurl's process-global allocator.
   Exercise their maximum ordinary overlap so transport memory remains
   bounded, recoverable, and large enough for the architecture it serves. */
static bool test_overlapping_scheduler_transport_pool(Budget *budget,
                                                       unsigned port)
{
    size_t baseline = budget->current;
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/large", port);
    FetchRequest request = {.method = "GET", .allow_http_errors = true};
    const size_t capacities[] = {1, 4, 4};
    FetchScheduler *schedulers[3] = {0};
    uint64_t ids[3][4] = {{0}};
    bool taken[3][4] = {{false}};
    size_t expected = 0;
    for (size_t group = 0; group < 3; group++) {
        schedulers[group] = fetch_scheduler_create(
            budget, capacities[group], capacities[group] * 64u * 1024u);
        CHECK(schedulers[group] != NULL);
        for (size_t item = 0; item < capacities[group]; item++) {
            ids[group][item] = fetch_scheduler_enqueue(
                schedulers[group], url, &request, 64u * 1024u, 5000);
            CHECK(ids[group][item] != 0);
            expected++;
        }
    }

    size_t completed = 0;
    for (size_t pump = 0; pump < 5000 && completed < expected; pump++) {
        for (size_t group = 0; group < 3; group++) {
            (void) fetch_scheduler_pump(
                schedulers[group], capacities[group], 1);
            for (size_t item = 0; item < capacities[group]; item++) {
                if (taken[group][item]) continue;
                bool success = false;
                FetchResult result = {.budget = budget};
                if (!fetch_scheduler_take(
                        schedulers[group], ids[group][item],
                        &success, &result)) continue;
                taken[group][item] = true;
                completed++;
                CHECK(success && result.status_code == 200
                      && result.length == 48u * 1024u);
                fetch_result_destroy(&result);
            }
        }
    }
    CHECK(completed == expected);
    for (size_t group = 0; group < 3; group++) {
        fetch_scheduler_destroy(schedulers[group]);
    }
    return budget->current == baseline;
}
