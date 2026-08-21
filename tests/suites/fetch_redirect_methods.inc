static bool test_scheduled_redirect(Budget *budget, unsigned port)
{
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/same", port);
    BrowserSession browser;
    CHECK(browser_session_init(&browser, budget, 64 * 1024)
          && browser_session_cookie_set_http(
              &browser, url, "secret=scheduled; Path=/"));
    TilefinchRequestContext cookie_context = {
        .target_url = url, .initiator_url = url, .top_level_url = url,
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_DOCUMENT,
        .top_level_navigation = true
    };
    FetchRequest request = {
        .method = "GET", .cookie = "secret=scheduled",
        .origin = "http://origin.example",
        .allow_http_errors = true,
        .cookie_session = &browser,
        .cookie_context = &cookie_context
    };
    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 2, 128 * 1024);
    CHECK(scheduler != NULL);
    uint64_t id = fetch_scheduler_enqueue(
        scheduler, url, &request, 64 * 1024, 5000);
    CHECK(id != 0);
    FetchResult result = {.budget = budget};
    bool success = false, taken = false;
    for (size_t pump = 0; pump < 2000 && !taken; pump++) {
        (void) fetch_scheduler_pump(scheduler, 1, 4);
        taken = fetch_scheduler_take(scheduler, id, &success, &result);
    }
    CHECK(taken && success && result.status_code == 200
          && strstr(result.data, "secret=scheduled") != NULL
          && strstr(result.data, "hop=seen") != NULL
          && strstr(result.data, "origin=http://origin.example") != NULL
          && result.set_cookie_count == 2
          && strstr(fetch_set_cookie_url(&result, 0, ""), "/same") != NULL
          && strstr(fetch_set_cookie_url(&result, 1, ""), "/final") != NULL);
    fetch_result_destroy(&result);

    snprintf(url, sizeof(url), "http://127.0.0.1:%u/cross", port);
    request = (FetchRequest) {
        .method = "GET", .cookie = "secret=scheduled-cross",
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
    id = fetch_scheduler_enqueue(
        scheduler, url, &request, 64 * 1024, 5000);
    CHECK(id != 0);
    result = (FetchResult) {.budget = budget};
    success = false;
    taken = false;
    for (size_t pump = 0; pump < 2000 && !taken; pump++) {
        (void) fetch_scheduler_pump(scheduler, 1, 4);
        taken = fetch_scheduler_take(scheduler, id, &success, &result);
    }
    CHECK(taken && success && result.status_code == 200
          && strstr(result.data, "cookie=<none>") != NULL
          && strstr(result.data, "authorization=<none>") != NULL
          && strstr(result.data, "origin=null") != NULL
          && strstr(result.data, "x-public=retained") != NULL
          && strstr(result.data, "referer=<none>") != NULL
          && strstr(result.data, "fetch-site=cross-site") != NULL);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    browser_session_destroy(&browser);
    return true;
}

static bool test_redirect_methods(Budget *budget, unsigned port)
{
    static const struct {
        const char *path;
        const char *expected_method;
        const char *expected_body;
        const char *expected_content_type;
        bool payload_absent;
    } cases[] = {
        {"post302", "method=GET", "body=", "content-type=<none>", true},
        {"post307", "method=POST", "body=payload",
         "content-type=text/plain", false}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char url[256];
        snprintf(url, sizeof(url), "http://127.0.0.1:%u/%s", port,
                 cases[i].path);
        FetchRequest request = {
            .method = "POST", .body = "payload", .body_length = 7,
            .content_type = "text/plain", .allow_http_errors = true
        };
        FetchResult result = {.budget = budget};
        bool fetched = fetch_redirect(budget, url, &request, &result);
        if (!fetched || result.status_code != 200 || result.data == NULL
            || strstr(result.data, cases[i].expected_method) == NULL
            || strstr(result.data, cases[i].expected_body) == NULL
            || strstr(result.data, cases[i].expected_content_type) == NULL
            || (cases[i].payload_absent
                && strstr(result.data, "payload") != NULL)) {
            fprintf(stderr,
                    "redirect method case=%s fetched=%d status=%ld body=%s error=%s\n",
                    cases[i].path, fetched, result.status_code,
                    result.data == NULL ? "<null>" : result.data,
                    result.error);
            fetch_result_destroy(&result);
            return false;
        }
        fetch_result_destroy(&result);
    }
    return true;
}

static bool test_generic_method_bodies_and_head(Budget *budget,
                                                unsigned port)
{
    static const struct {
        const char *method;
        const char *path;
    } body_cases[] = {
        {"PUT", "method"},
        {"PATCH", "method"},
        {"PUT", "custom307"}
    };
    char url[256];
    for (size_t i = 0; i < sizeof(body_cases) / sizeof(body_cases[0]); i++) {
        snprintf(url, sizeof(url), "http://127.0.0.1:%u/%s", port,
                 body_cases[i].path);
        FetchRequest request = {
            .method = body_cases[i].method,
            .body = "payload", .body_length = 7,
            .content_type = "text/plain", .allow_http_errors = true
        };
        FetchResult result = {.budget = budget};
        CHECK(fetch_redirect(budget, url, &request, &result)
              && strstr(result.data, body_cases[i].method) != NULL
              && strstr(result.data, "body=payload") != NULL
              && strstr(result.data, "content-type=text/plain") != NULL);
        fetch_result_destroy(&result);
    }

    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64 * 1024);
    CHECK(scheduler != NULL);
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/method", port);
    FetchRequest put = {
        .method = "PUT", .body = "scheduled", .body_length = 9,
        .content_type = "text/plain", .allow_http_errors = true
    };
    FetchResult result = {.budget = budget};
    CHECK(fetch_scheduler_request(
              scheduler, url, &put, 64 * 1024, 1000, &result)
          && strstr(result.data, "method=PUT") != NULL
          && strstr(result.data, "body=scheduled") != NULL);
    fetch_result_destroy(&result);

    snprintf(url, sizeof(url), "http://127.0.0.1:%u/head", port);
    FetchRequest head = {
        .method = "HEAD", .allow_http_errors = true
    };
    CHECK(fetch_scheduler_request(
              scheduler, url, &head, 64 * 1024, 1000, &result)
          && result.status_code == 200 && result.length == 0);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);

    result = (FetchResult) {.budget = budget};
    CHECK(fetch_redirect(budget, url, &head, &result)
          && result.status_code == 200 && result.length == 0);
    fetch_result_destroy(&result);
    return true;
}

static bool test_fetch_site_none_survives_redirects(Budget *budget,
                                                     unsigned port)
{
    static const char *paths[] = {"same", "cross"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        char url[256];
        snprintf(url, sizeof(url), "http://127.0.0.1:%u/%s", port,
                 paths[i]);
        FetchRequest request = {
            .method = "GET", .allow_http_errors = true,
            .sec_fetch_site = "none", .initiator_url = url
        };
        FetchResult result = {.budget = budget};
        CHECK(fetch_redirect(budget, url, &request, &result)
              && strstr(result.data, "fetch-site=none") != NULL);
        fetch_result_destroy(&result);
    }

    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/cross", port);
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true,
        .sec_fetch_site = "none", .initiator_url = url
    };
    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64 * 1024);
    FetchResult result = {.budget = budget};
    CHECK(scheduler != NULL
          && fetch_scheduler_request(
              scheduler, url, &request, 64 * 1024, 1000, &result)
          && strstr(result.data, "fetch-site=none") != NULL);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    return true;
}

static bool test_redirect_limit(Budget *budget, unsigned port)
{
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/loop", port);
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true,
        .initiator_url = url
    };
    FetchResult result = {.budget = budget};
    bool fetched = fetch_redirect(budget, url, &request, &result);
    bool ok = !fetched
        && strstr(result.error, "redirect loop detected") != NULL;
    fetch_result_destroy(&result);
    CHECK(ok);

    /* Six redirects exceeded the old compatibility bound.  Eight remain
       bounded and useful for federated sign-in/front-door chains; a ninth
       must still fail closed. */
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/redirect-chain/8", port);
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_redirect(budget, url, &request, &result)
          && result.status_code == 200
          && strcmp(result.data, "redirect-chain-final") == 0);
    fetch_result_destroy(&result);

    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/redirect-chain/9", port);
    result = (FetchResult) {.budget = budget};
    CHECK(!fetch_redirect(budget, url, &request, &result)
          && strstr(result.error, "eight-hop redirect limit") != NULL);
    fetch_result_destroy(&result);
    return true;
}

static bool test_client_hint_redirect_scope(Budget *budget,
                                            unsigned source_port,
                                            unsigned target_port)
{
    char url[256], hint_origin[256];
    snprintf(hint_origin, sizeof(hint_origin),
             "http://127.0.0.1:%u", source_port);
    FetchRequestValidationError validation = FETCH_REQUEST_VALIDATION_OK;
    FetchRequest injected = {
        .method = "GET",
        .extra_headers = "Sec-CH-UA-Arch: \"must-not-bypass-scope\""
    };
    CHECK(!fetch_request_validate(&injected, &validation)
          && validation == FETCH_REQUEST_VALIDATION_EXTRA_HEADERS);
    static const char *target_hosts[] = {"127.0.0.1", "localhost"};
    for (size_t i = 0;
         i < sizeof(target_hosts) / sizeof(target_hosts[0]); i++) {
        snprintf(url, sizeof(url),
                 "http://127.0.0.1:%u/hint-retry?target_port=%u&target_host=%s",
                 source_port, target_port, target_hosts[i]);
        FetchRequest request = {
            .method = "GET", .allow_http_errors = true,
            .send_client_hints = true,
            .client_hint_tokens = "Sec-CH-UA-Arch",
            .client_hint_origin = hint_origin,
            .send_low_client_hints = true
        };
        FetchResult result = {.budget = budget};
        CHECK(fetch_redirect(budget, url, &request, &result)
              && result.status_code == 200
              && strstr(result.data, "subset=1") != NULL
              && strstr(result.data, "terminal-high=0") != NULL
              && strstr(result.data, "terminal-low=1") != NULL);
        fetch_result_destroy(&result);
    }

    FetchRequest request = {
        .method = "GET", .allow_http_errors = true,
        .send_client_hints = true,
        .client_hint_tokens = "Sec-CH-UA-Arch",
        .client_hint_origin = hint_origin,
        .send_low_client_hints = true
    };
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/hint-retry?target_port=%u&target_host=127.0.0.1&bounce=1",
             source_port, target_port);
    FetchResult result = {.budget = budget};
    CHECK(fetch_redirect(budget, url, &request, &result)
          && strstr(result.data, "subset=1") != NULL
          && strstr(result.data, "terminal-high=0") != NULL
          && strstr(result.data, "terminal-low=1") != NULL);
    fetch_result_destroy(&result);

    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/hint-retry?target_port=%u&target_host=127.0.0.1",
             source_port, target_port);
    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64 * 1024);
    CHECK(scheduler != NULL);
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_scheduler_request(
              scheduler, url, &request, 64 * 1024, 5000, &result)
          && strstr(result.data, "subset=1") != NULL
          && strstr(result.data, "terminal-high=0") != NULL
          && strstr(result.data, "terminal-low=1") != NULL);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    return true;
}

static bool test_navigation_critical_hint_redirect_scope(
    Budget *budget, unsigned source_port, unsigned hint_port)
{
    size_t baseline = budget->current;
    NavigationSession navigation;
    BrowserSession browser;
    CHECK(browser_session_init(&browser, budget, 64 * 1024)
          && navigation_init(&navigation, budget, 4));
    navigation_attach_browser_session(&navigation, &browser);
    navigation_enable_scripts(&navigation, 2 * MIB, 1000);
    navigation_enable_document_scripts(
        &navigation, 4, 32 * 1024, 16 * 1024, 1000);

    char source_url[256], start_url[256], hint_url[256];
    snprintf(source_url, sizeof(source_url),
             "http://127.0.0.1:%u/page", source_port);
    static const char prior[] =
        "<!doctype html><title>Hint scope prior</title><p>prior</p>";
    uint64_t generation = navigation_begin(&navigation);
    CHECK(navigation_commit_html(
        &navigation, generation, source_url, prior, sizeof(prior) - 1,
        480, NULL, NULL, true));
    snprintf(navigation.pending_navigation_referer,
             sizeof(navigation.pending_navigation_referer), "%s",
             source_url);
    snprintf(start_url, sizeof(start_url),
             "http://127.0.0.1:%u/critical-chain-start", source_port);
    generation = navigation_begin(&navigation);
    CHECK(navigation_load_url(
              &navigation, generation, start_url, 64 * 1024, 5000, 480,
              NULL, NULL, true)
          && navigation.client_hint_retries == 1
          && strcmp(navigation.page.document.title,
                    "Scoped Critical-CH final") == 0
          && strcmp(navigation.page.script_result.summary,
                    "critical-scope:1:1:1") == 0
          && strstr(navigation_current(&navigation)->url,
                    "http://localhost:")
                 == navigation_current(&navigation)->url);
    snprintf(hint_url, sizeof(hint_url),
             "http://127.0.0.1:%u/critical-chain", hint_port);
    char cookies[4096] = {0};
    CHECK(browser_session_cookie_get(
              &browser, hint_url, cookies, sizeof(cookies))
          && strstr(cookies, "retry_path=seen") != NULL
          && strstr(cookies, "discarded_hint_body=") == NULL);

    char no_accept_url[256];
    snprintf(no_accept_url, sizeof(no_accept_url),
             "http://127.0.0.1:%u/critical-without-accept", source_port);
    generation = navigation_begin(&navigation);
    bool loaded = navigation_load_url(
        &navigation, generation, no_accept_url,
        64 * 1024, 5000, 480, NULL, NULL, true);
    bool no_unaccepted_retry = loaded
        && navigation.client_hint_retries == 1
        && strcmp(navigation.page.document.title,
                  "No unaccepted retry") == 0
        && strcmp(navigation.page.script_result.summary,
                  "no-retry:0") == 0;
    if (!no_unaccepted_retry) {
        fprintf(stderr,
                "critical-without-accept loaded=%d retries=%zu title=\"%s\" "
                "summary=\"%s\" error=\"%s\"\n",
                loaded, navigation.client_hint_retries,
                navigation.page.document.title,
                navigation.page.script_result.summary,
                navigation.last_error);
    }
    CHECK(no_unaccepted_retry);
    navigation_destroy(&navigation);
    browser_session_destroy(&browser);
    return budget->current == baseline;
}

static bool test_navigation_redirect_fragment(Budget *budget, unsigned port)
{
    size_t baseline = budget->current;
    NavigationSession navigation;
    if (!navigation_init(&navigation, budget, 2)) return false;
    char url[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/nav-start#client", port);
    uint64_t generation = navigation_begin(&navigation);
    bool loaded = navigation_load_url(
        &navigation, generation, url, 64 * 1024, 5000, 480,
        NULL, NULL, true);
    const NavigationEntry *current = navigation_current(&navigation);
    bool ok = loaded && current != NULL
        && strstr(current->url, "/nav-final#server") != NULL
        && strcmp(navigation.page.document.title, "Redirected") == 0;
    navigation_destroy(&navigation);
    return ok && budget->current == baseline;
}
