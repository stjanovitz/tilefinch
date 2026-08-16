static bool trace_replay_rejects(Budget *budget, const char *trace_directory,
                                 const char *url,
                                 const FetchRequest *request)
{
    char error[256] = {0};
    FetchResult result = {.budget = budget};
    bool began = fetch_trace_replay_begin(
        trace_directory, error, sizeof(error));
    bool rejected = began
        && !fetch_redirect(budget, url, request, &result)
        && (strstr(result.error, "does not match") != NULL
            || strstr(result.error, "is corrupt") != NULL);
    bool cookie_storage_clean = response_cookie_storage_empty(&result);
    fetch_trace_end();
    fetch_result_destroy(&result);
    return rejected && cookie_storage_clean;
}

static bool replace_trace_byte(const char *path, const char *needle,
                               char replacement)
{
    FILE *file = fopen(path, "r+b");
    if (file == NULL) return false;
    char line[1024];
    bool replaced = false;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *match = strstr(line, needle);
        if (match == NULL) continue;
        long end = ftell(file);
        long offset = end - (long) strlen(line) + (long) (match - line);
        replaced = offset >= 0 && fseek(file, offset, SEEK_SET) == 0
            && fputc(replacement, file) != EOF;
        break;
    }
    return fclose(file) == 0 && replaced;
}

static bool replace_trace_byte_at_offset(const char *path,
                                         const char *needle,
                                         size_t needle_offset,
                                         char replacement)
{
    if (needle == NULL || needle_offset >= strlen(needle)) return false;
    FILE *file = fopen(path, "r+b");
    if (file == NULL) return false;
    char line[1024];
    bool replaced = false;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *match = strstr(line, needle);
        if (match == NULL) continue;
        long end = ftell(file);
        long offset = end - (long) strlen(line)
            + (long) (match - line) + (long) needle_offset;
        replaced = offset >= 0 && fseek(file, offset, SEEK_SET) == 0
            && fputc(replacement, file) != EOF;
        break;
    }
    return fclose(file) == 0 && replaced;
}

static bool trace_meta_contains(const char *trace_directory,
                                const char *expected)
{
    char path[4096];
    int written = snprintf(path, sizeof(path), "%s/0000.meta",
                           trace_directory);
    if (written < 0 || (size_t) written >= sizeof(path)) return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strcmp(line, expected) == 0) {
            found = true;
            break;
        }
    }
    return fclose(file) == 0 && found;
}

static bool trace_info_contains(const char *trace_directory,
                                const char *needle)
{
    char path[4096];
    int written = snprintf(path, sizeof(path), "%s/trace.meta",
                           trace_directory);
    if (written < 0 || (size_t) written >= sizeof(path)) return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, needle) != NULL) {
            found = true;
            break;
        }
    }
    return fclose(file) == 0 && found;
}

static bool trace_file_exists(const char *trace_directory,
                              const char *name)
{
    char path[4096];
    int written = snprintf(path, sizeof(path), "%s/%s",
                           trace_directory, name);
    if (written < 0 || (size_t) written >= sizeof(path)) return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    return fclose(file) == 0;
}

static bool trace_remove_file(const char *trace_directory,
                              const char *name)
{
    char path[4096];
    int written = snprintf(path, sizeof(path), "%s/%s",
                           trace_directory, name);
    return written > 0 && (size_t) written < sizeof(path)
        && remove(path) == 0;
}

static bool test_single_hop_trace_acquisition(
    Budget *budget, unsigned port, const char *trace_directory)
{
    size_t baseline = budget->current;
    char url[256], cookie_url_line[320], error[256] = {0};
    int url_written = snprintf(
        url, sizeof(url), "http://127.0.0.1:%u/same", port);
    int cookie_written = snprintf(
        cookie_url_line, sizeof(cookie_url_line),
        "set-cookie-url-0=%s\n", url);
    CHECK(url_written > 0 && (size_t) url_written < sizeof(url)
          && cookie_written > 0
          && (size_t) cookie_written < sizeof(cookie_url_line));
    /* No request cookie or authentication is supplied. INCLUDE is used only
       so the response Set-Cookie shape reaches redacted trace capture. */
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true,
        .credentials = FETCH_CREDENTIALS_INCLUDE
    };
    FetchResult result = {.budget = budget};
    CHECK(fetch_trace_capture_begin(
              trace_directory, error, sizeof(error))
          && fetch_request_single_hop(
              budget, url, &request, 64u * 1024u, 5000, &result)
          && result.status_code == 302
          && result.set_cookie_count == 1
          && strcmp(result.set_cookies[0], "hop=seen; Path=/") == 0);
    fetch_result_destroy(&result);
    fetch_trace_end();
    CHECK(trace_info_contains(trace_directory, "capture-complete=yes")
          && trace_info_contains(trace_directory, "record-count=1")
          && trace_meta_contains(trace_directory, "cookie-values=redacted\n")
          && trace_meta_contains(trace_directory, "request-cookie-bytes=0\n")
          && trace_meta_contains(
                 trace_directory, "request-has-cf-clearance=0\n")
          && trace_meta_contains(trace_directory, "request-credentials=0\n")
          && trace_meta_contains(trace_directory, "status=302\n")
          && trace_meta_contains(trace_directory, "set-cookie-count=1\n")
          && trace_meta_contains(
                 trace_directory, "set-cookie-0=hop=xxxx; Path=/\n")
          && trace_meta_contains(trace_directory, cookie_url_line));
    CHECK(trace_remove_file(trace_directory, "0000.meta")
          && trace_remove_file(trace_directory, "0000.body")
          && trace_remove_file(trace_directory, "trace.meta"));
    return budget->current == baseline;
}

static bool trace_append_duplicate_seed(const char *trace_directory)
{
    char path[4096];
    int written = snprintf(path, sizeof(path), "%s/trace.meta",
                           trace_directory);
    if (written < 0 || (size_t) written >= sizeof(path)) return false;
    FILE *file = fopen(path, "ab");
    bool ok = file != NULL
        && fprintf(file, "cookie-seed-0-name=duplicate\n") >= 0;
    if (file != NULL) ok = fclose(file) == 0 && ok;
    return ok;
}

static bool trace_append_embedded_nul(const char *trace_directory)
{
    char path[4096];
    int written = snprintf(path, sizeof(path), "%s/trace.meta",
                           trace_directory);
    if (written < 0 || (size_t) written >= sizeof(path)) return false;
    static const unsigned char hidden[] =
        "legacy-extension=visible\0cookie-seed-version=1\n";
    FILE *file = fopen(path, "ab");
    bool ok = file != NULL
        && fwrite(hidden, 1, sizeof(hidden) - 1, file)
               == sizeof(hidden) - 1;
    if (file != NULL) ok = fclose(file) == 0 && ok;
    return ok;
}

static bool trace_rewrite_info_as_legacy(const char *trace_directory)
{
    char path[4096], temporary[4096], origin[128] = {0};
    int path_written = snprintf(path, sizeof(path), "%s/trace.meta",
                                trace_directory);
    int temporary_written = snprintf(
        temporary, sizeof(temporary), "%s/trace.meta.tmp", trace_directory);
    if (path_written < 0 || (size_t) path_written >= sizeof(path)
        || temporary_written < 0
        || (size_t) temporary_written >= sizeof(temporary)) return false;
    FILE *input = fopen(path, "rb");
    char line[1024];
    while (input != NULL && fgets(line, sizeof(line), input) != NULL) {
        if (strncmp(line, "origin-ms=", 10) == 0) {
            snprintf(origin, sizeof(origin), "%s", line);
            break;
        }
    }
    bool ok = input != NULL && !ferror(input) && origin[0] != '\0';
    if (input != NULL) ok = fclose(input) == 0 && ok;
    FILE *output = ok ? fopen(temporary, "wb") : NULL;
    ok = output != NULL
        && fprintf(output,
                   "psp-http-trace-clock=1\n%slegacy-extension\n\n",
                   origin) >= 0;
    if (output != NULL) ok = fclose(output) == 0 && ok;
    if (ok) ok = rename(temporary, path) == 0;
    if (!ok) (void) remove(temporary);
    return ok;
}

static bool trace_rewrite_record_version(
    const char *trace_directory, size_t sequence,
    const char *from_version, const char *to_version)
{
    char path[4096], temporary[4096], line[9000], expected[64];
    int path_written = snprintf(
        path, sizeof(path), "%s/%04zu.meta", trace_directory, sequence);
    int temporary_written = snprintf(
        temporary, sizeof(temporary), "%s/%04zu.meta.tmp",
        trace_directory, sequence);
    int expected_written = snprintf(
        expected, sizeof(expected), "psp-http-trace=%s\n", from_version);
    if (path_written < 0 || (size_t) path_written >= sizeof(path)
        || temporary_written < 0
        || (size_t) temporary_written >= sizeof(temporary)
        || expected_written < 0
        || (size_t) expected_written >= sizeof(expected)) return false;
    FILE *input = fopen(path, "rb");
    FILE *output = input == NULL ? NULL : fopen(temporary, "wb");
    bool ok = output != NULL;
    bool version_seen = false;
    while (ok && fgets(line, sizeof(line), input) != NULL) {
        if (strcmp(line, expected) == 0) {
            version_seen = true;
            ok = fprintf(output, "psp-http-trace=%s\n", to_version) >= 0;
        } else {
            ok = fputs(line, output) >= 0;
        }
    }
    ok = ok && version_seen && !ferror(input);
    if (input != NULL) ok = fclose(input) == 0 && ok;
    if (output != NULL) ok = fclose(output) == 0 && ok;
    if (ok) ok = rename(temporary, path) == 0;
    if (!ok) (void) remove(temporary);
    return ok;
}

static bool trace_downgrade_record_v11_to_v8(
    const char *trace_directory, size_t sequence)
{
    char path[4096], temporary[4096], line[9000];
    int path_written = snprintf(
        path, sizeof(path), "%s/%04zu.meta", trace_directory, sequence);
    int temporary_written = snprintf(
        temporary, sizeof(temporary), "%s/%04zu.meta.tmp",
        trace_directory, sequence);
    if (path_written < 0 || (size_t) path_written >= sizeof(path)
        || temporary_written < 0
        || (size_t) temporary_written >= sizeof(temporary)) return false;
    FILE *input = fopen(path, "rb");
    FILE *output = input == NULL ? NULL : fopen(temporary, "wb");
    bool ok = output != NULL;
    bool version_seen = false;
    while (ok && fgets(line, sizeof(line), input) != NULL) {
        if (strcmp(line, "psp-http-trace=11\n") == 0) {
            version_seen = true;
            ok = fputs("psp-http-trace=8\n", output) >= 0;
        } else if (strncmp(line, "logical-request-url=", 20) == 0
                   || strncmp(
                          line,
                          "response-referrer-policy-metadata-valid=", 40)
                          == 0
                   || strncmp(
                          line, "response-referrer-policy-present=", 33)
                          == 0
                   || strncmp(
                          line, "response-referrer-policy=", 25) == 0
                   || strncmp(
                          line, "response-security-headers-truncated=",
                          sizeof("response-security-headers-truncated=") - 1)
                          == 0
                   || strncmp(line, "response-security-metadata=",
                              sizeof("response-security-metadata=") - 1)
                          == 0
                   || strncmp(line, "response-security-allow-origin=",
                              sizeof("response-security-allow-origin=") - 1)
                          == 0) {
            continue;
        } else {
            ok = fputs(line, output) >= 0;
        }
    }
    ok = ok && version_seen && !ferror(input);
    if (input != NULL) ok = fclose(input) == 0 && ok;
    if (output != NULL) ok = fclose(output) == 0 && ok;
    if (ok) ok = rename(temporary, path) == 0;
    if (!ok) (void) remove(temporary);
    return ok;
}

static bool trace_scheduler_wait_take(FetchScheduler *scheduler,
                                      uint64_t id, bool *success,
                                      FetchResult *result)
{
    if (scheduler == NULL || id == 0 || success == NULL || result == NULL) {
        return false;
    }
    for (size_t pump = 0; pump < 2000; pump++) {
        if (fetch_scheduler_take(scheduler, id, success, result)) return true;
        (void) fetch_scheduler_pump(scheduler, 1, 4);
    }
    return false;
}

static bool test_trace_logical_url_and_referrer_metadata(
    Budget *budget, unsigned port, const char *trace_directory)
{
    size_t baseline = budget->current;
    char sync_url[256], normalized_sync_url[256], different_fragment[256];
    char scheduled_url[256];
    char scheduled_mismatch[256], legacy_url[256], legacy_fragment[256];
    char logical_line[320], error[256] = {0};
    snprintf(sync_url, sizeof(sync_url),
             "HTTP://127.0.0.1:%u/referrer-policy-filled-final#alpha",
             port);
    snprintf(normalized_sync_url, sizeof(normalized_sync_url),
             "http://127.0.0.1:%u/referrer-policy-filled-final#alpha",
             port);
    snprintf(different_fragment, sizeof(different_fragment),
             "http://127.0.0.1:%u/referrer-policy-filled-final#beta",
             port);
    snprintf(scheduled_url, sizeof(scheduled_url),
             "http://127.0.0.1:%u/final?trace=scheduled#alpha", port);
    snprintf(scheduled_mismatch, sizeof(scheduled_mismatch),
             "http://127.0.0.1:%u/final?trace=scheduled#beta", port);
    snprintf(legacy_url, sizeof(legacy_url),
             "http://127.0.0.1:%u/final?trace=legacy", port);
    snprintf(legacy_fragment, sizeof(legacy_fragment),
             "http://127.0.0.1:%u/final?trace=legacy#client", port);
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true
    };

    FetchResult result = {.budget = budget};
    CHECK(fetch_trace_capture_begin(trace_directory, error, sizeof(error))
          && fetch_redirect(budget, sync_url, &request, &result));
    bool present = false;
    char policy[FETCH_REFERRER_POLICY_LIMIT];
    CHECK(fetch_response_referrer_policy(&result, &present, policy)
          && present && strcmp(policy, "no-referrer") == 0);
    fetch_result_destroy(&result);
    fetch_trace_end();
    snprintf(logical_line, sizeof(logical_line),
             "logical-request-url=%s\n", normalized_sync_url);
    CHECK(trace_meta_contains(trace_directory, "psp-http-trace=11\n")
          && trace_meta_contains(trace_directory, logical_line)
          && trace_meta_contains(
                 trace_directory,
                 "response-referrer-policy-metadata-valid=1\n")
          && trace_meta_contains(
                 trace_directory,
                 "response-referrer-policy-present=1\n")
          && trace_meta_contains(
                 trace_directory,
                 "response-referrer-policy=no-referrer\n")
          && trace_meta_contains(
                 trace_directory,
                 "response-security-headers-truncated=0\n"));

    result = (FetchResult) {.budget = budget};
    CHECK(fetch_trace_replay_begin(trace_directory, error, sizeof(error))
          && fetch_redirect(budget, sync_url, &request, &result)
          && fetch_response_referrer_policy(&result, &present, policy)
          && present && strcmp(policy, "no-referrer") == 0);
    fetch_result_destroy(&result);
    fetch_trace_end();

    char security_meta_path[4096];
    int security_meta_written = snprintf(
        security_meta_path, sizeof(security_meta_path), "%s/0000.meta",
        trace_directory);
    static const char security_clear[] =
        "response-security-headers-truncated=0";
    const size_t security_value = sizeof(security_clear) - 2;
    CHECK(security_meta_written > 0
          && (size_t) security_meta_written < sizeof(security_meta_path)
          && replace_trace_byte_at_offset(
                 security_meta_path, security_clear, security_value, '1'));
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_trace_replay_begin(trace_directory, error, sizeof(error))
          && fetch_redirect(budget, sync_url, &request, &result)
          && result.response_security_headers_truncated);
    fetch_result_destroy(&result);
    fetch_trace_end();
    CHECK(replace_trace_byte_at_offset(
        security_meta_path, "response-security-headers-truncated=1",
        security_value, '0'));
    PolicyStreamProbe replay_probe = {0};
    FetchStreamOptions replay_stream = {
        .on_headers = policy_stream_headers,
        .on_body = policy_stream_body,
        .opaque = &replay_probe
    };
    FetchStreamMetrics replay_metrics = {0};
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_trace_replay_begin(trace_directory, error, sizeof(error))
          && fetch_request_stream_cancelable(
                 budget, sync_url, &request, 64 * 1024, 5000,
                 NULL, NULL, &replay_stream, &replay_metrics, &result)
          && replay_probe.header_calls == 1 && replay_probe.policy_ok
          && replay_probe.body_bytes == strlen("filled-policy-final"));
    fetch_result_destroy(&result);
    fetch_trace_end();
    CHECK(trace_replay_rejects(
        budget, trace_directory, different_fragment, &request));

    /* The scheduler must retain the logical URL after enqueue returns; the
       capture is finalized later, after asynchronous transport completion. */
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_trace_capture_begin(trace_directory, error, sizeof(error))
          && fetch_scheduled(
                 budget, scheduled_url, &request, &result));
    fetch_result_destroy(&result);
    fetch_trace_end();
    snprintf(logical_line, sizeof(logical_line),
             "logical-request-url=%s\n", scheduled_url);
    CHECK(trace_meta_contains(trace_directory, logical_line));
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_trace_replay_begin(trace_directory, error, sizeof(error))
          && fetch_scheduled(
                 budget, scheduled_url, &request, &result));
    fetch_result_destroy(&result);
    fetch_trace_end();
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_trace_replay_begin(trace_directory, error, sizeof(error))
          && !fetch_scheduled(
                 budget, scheduled_mismatch, &request, &result)
          && strstr(result.error, "does not match") != NULL);
    fetch_result_destroy(&result);
    fetch_trace_end();

    /* A genuine v8-shaped record has no logical-URL or direct-policy fields.
       It remains usable for fragmentless requests only. */
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_trace_capture_begin(trace_directory, error, sizeof(error))
          && fetch_redirect(budget, legacy_url, &request, &result));
    fetch_result_destroy(&result);
    fetch_trace_end();
    CHECK(trace_downgrade_record_v11_to_v8(trace_directory, 0));
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_trace_replay_begin(trace_directory, error, sizeof(error))
          && fetch_redirect(budget, legacy_url, &request, &result));
    fetch_result_destroy(&result);
    fetch_trace_end();

    CHECK(trace_replay_rejects(
        budget, trace_directory, legacy_fragment, &request));

    /* A legacy record captured under an otherwise-identical typed request
       cannot prove that its bounded response-header snapshot retained every
       security-relevant field. It may replay its bytes, but the synthetic
       truncation state must make the typed grant fail closed. */
    TilefinchRequestContext typed_context = {
        .target_url = legacy_url,
        .initiator_url = legacy_url,
        .top_level_url = legacy_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_SCRIPT
    };
    FetchRequest typed_transport = {.allow_http_errors = true};
    FetchPreparedPageRequest typed_prepared;
    CHECK(fetch_prepare_page_request_context(
        &typed_context, legacy_url, NULL, NULL, NULL, NULL,
        &typed_transport, &typed_prepared, NULL));
    const FetchRequest *typed_request = fetch_prepared_page_request(
        &typed_prepared);
    CHECK(typed_request != NULL);
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_trace_capture_begin(trace_directory, error, sizeof(error))
          && fetch_redirect(budget, legacy_url, typed_request, &result));
    fetch_result_destroy(&result);
    fetch_trace_end();
    CHECK(trace_rewrite_record_version(
        trace_directory, 0, "11", "9"));
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_trace_replay_begin(trace_directory, error, sizeof(error))
          && fetch_redirect(budget, legacy_url, typed_request, &result)
          && result.response_security_headers_truncated);
    TilefinchResourceGrant legacy_grant;
    TilefinchResourceDeniedReason legacy_denied;
    CHECK(!fetch_resource_grant_create(
              &result, &typed_context, false, true, false,
              &legacy_grant, &legacy_denied)
          && legacy_denied
                 == TILEFINCH_RESOURCE_DENIED_TRUNCATED_SECURITY_HEADER);
    fetch_result_destroy(&result);
    fetch_trace_end();
    return budget->current == baseline;
}

static bool test_trace_reordered_repeated_and_cancelled(
    Budget *budget, unsigned port, const char *trace_directory)
{
    size_t baseline = budget->current;
    char url_a[256], url_b[256], url_preload[256], error[256] = {0};
    snprintf(url_a, sizeof(url_a),
             "http://127.0.0.1:%u/final?identity=repeated", port);
    snprintf(url_b, sizeof(url_b),
             "http://127.0.0.1:%u/final?identity=reordered", port);
    snprintf(url_preload, sizeof(url_preload),
             "http://127.0.0.1:%u/final?identity=preload", port);
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true
    };
    CHECK(fetch_trace_capture_begin(
              trace_directory, error, sizeof(error))
          && !trace_file_exists(trace_directory, "trace.meta")
          && trace_file_exists(trace_directory, "trace.meta.capture"));
    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 3, 3u * 64u * 1024u);
    CHECK(scheduler != NULL);

    uint64_t cancelled = fetch_scheduler_enqueue(
        scheduler, url_a, &request, 64 * 1024, 5000);
    CHECK(cancelled != 0
          && fetch_scheduler_cancel(
                 scheduler, cancelled, "captured explicit cancellation"));
    bool success = true;
    FetchResult result = {.budget = budget};
    CHECK(fetch_scheduler_take(
              scheduler, cancelled, &success, &result)
          && !success);
    fetch_result_destroy(&result);

    uint64_t reordered = fetch_scheduler_enqueue(
        scheduler, url_b, &request, 64 * 1024, 5000);
    uint64_t repeated = fetch_scheduler_enqueue(
        scheduler, url_a, &request, 64 * 1024, 5000);
    result = (FetchResult) {.budget = budget};
    CHECK(trace_scheduler_wait_take(
              scheduler, reordered, &success, &result)
          && success && strstr(result.data, "query=identity=reordered") != NULL);
    fetch_result_destroy(&result);
    result = (FetchResult) {.budget = budget};
    CHECK(trace_scheduler_wait_take(
              scheduler, repeated, &success, &result)
          && success && strstr(result.data, "query=identity=repeated") != NULL);
    fetch_result_destroy(&result);

    /* Model a speculative preload discarded with its owner. Destruction must
       finalize the already assigned record as an external cancellation. */
    uint64_t preload = fetch_scheduler_enqueue(
        scheduler, url_preload, &request, 64 * 1024, 5000);
    CHECK(preload != 0);
    fetch_scheduler_destroy(scheduler);
    CHECK(!trace_file_exists(trace_directory, "trace.meta"));
    fetch_trace_end();
    CHECK(trace_file_exists(trace_directory, "trace.meta")
          && !trace_file_exists(trace_directory, "trace.meta.capture")
          && trace_info_contains(trace_directory, "capture-complete=yes")
          && trace_info_contains(trace_directory, "record-count=4")
          && trace_meta_contains(trace_directory, "external-cancel=1\n"));

    CHECK(fetch_trace_replay_begin(
        trace_directory, error, sizeof(error)));
    scheduler = fetch_scheduler_create(budget, 2, 2u * 64u * 1024u);
    CHECK(scheduler != NULL);
    /* An unrelated request may enqueue before the earlier cancelled record. */
    reordered = fetch_scheduler_enqueue(
        scheduler, url_b, &request, 64 * 1024, 5000);
    result = (FetchResult) {.budget = budget};
    CHECK(trace_scheduler_wait_take(
              scheduler, reordered, &success, &result)
          && success && strstr(result.data, "query=identity=reordered") != NULL);
    fetch_result_destroy(&result);

    /* Repeated identical requests retain occurrence order: the first still
       waits for a real cancellation, while the second replays its success. */
    cancelled = fetch_scheduler_enqueue(
        scheduler, url_a, &request, 64 * 1024, 5000);
    CHECK(cancelled != 0
          && fetch_scheduler_cancel(
                 scheduler, cancelled, "replayed explicit cancellation"));
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_scheduler_take(
              scheduler, cancelled, &success, &result)
          && !success && result.trace_external_cancel);
    fetch_result_destroy(&result);
    repeated = fetch_scheduler_enqueue(
        scheduler, url_a, &request, 64 * 1024, 5000);
    result = (FetchResult) {.budget = budget};
    CHECK(trace_scheduler_wait_take(
              scheduler, repeated, &success, &result)
          && success && strstr(result.data, "query=identity=repeated") != NULL);
    fetch_result_destroy(&result);

    preload = fetch_scheduler_enqueue(
        scheduler, url_preload, &request, 64 * 1024, 5000);
    CHECK(preload != 0);
    fetch_scheduler_destroy(scheduler);
    /* Enqueue claimed the teardown-cancel record. It cannot be replayed as a
       second success or cancellation for the same request. */
    scheduler = fetch_scheduler_create(budget, 1, 64 * 1024);
    CHECK(scheduler != NULL);
    preload = fetch_scheduler_enqueue(
        scheduler, url_preload, &request, 64 * 1024, 5000);
    result = (FetchResult) {.budget = budget};
    CHECK(trace_scheduler_wait_take(
              scheduler, preload, &success, &result)
          && !success
          && strstr(result.error, "does not match") != NULL);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    return budget->current == baseline;
}

static bool test_trace_dynamic_source_context_and_legacy_inventory(
    Budget *budget, unsigned port, const char *trace_directory)
{
    size_t baseline = budget->current;
    char url[256], changed_url[256], stale_url[256], origin[128];
    char captured_context[256], replayed_context[256];
    char referer[256], changed_referer[256], error[256] = {0};
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/final?request=stable", port);
    snprintf(changed_url, sizeof(changed_url),
             "http://127.0.0.1:%u/final?request=changed", port);
    snprintf(stale_url, sizeof(stale_url),
             "http://127.0.0.1:%u/final?identity=reordered", port);
    snprintf(origin, sizeof(origin), "http://127.0.0.1:%u", port);
    snprintf(captured_context, sizeof(captured_context),
             "%s/context?nonce=captured", origin);
    snprintf(replayed_context, sizeof(replayed_context),
             "%s/context?nonce=replayed", origin);
    snprintf(referer, sizeof(referer), "%s/source", origin);
    snprintf(changed_referer, sizeof(changed_referer),
             "%s/other-source", origin);
    FetchRequest captured_request = {
        .method = "GET", .allow_http_errors = true,
        .referer = referer, .origin = origin,
        .accept = "text/plain,*/*;q=0.1",
        .sec_fetch_dest = "empty", .sec_fetch_mode = "cors",
        .sec_fetch_site = "same-origin",
        .credentials = FETCH_CREDENTIALS_INCLUDE,
        .credential_origin = captured_context,
        .initiator_url = captured_context,
        .referrer_source = captured_context,
        .referrer_policy = "strict-origin-when-cross-origin"
    };
    FetchResult result = {.budget = budget};
    CHECK(fetch_trace_capture_begin(trace_directory, error, sizeof(error))
          && fetch_redirect(budget, url, &captured_request, &result));
    fetch_result_destroy(&result);
    fetch_trace_end();
    CHECK(trace_info_contains(trace_directory, "record-count=1")
          /* Files from the preceding four-record corpus deliberately remain;
             the finalized count, rather than directory contents, is authority. */
          && trace_file_exists(trace_directory, "0001.meta"));

    FetchRequest replayed_request = captured_request;
    replayed_request.credential_origin = replayed_context;
    replayed_request.initiator_url = replayed_context;
    replayed_request.referrer_source = replayed_context;
    CHECK(fetch_trace_replay_begin(trace_directory, error, sizeof(error)));
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_redirect(budget, url, &replayed_request, &result)
          && strstr(result.data, "query=request=stable") != NULL);
    fetch_result_destroy(&result);
    fetch_trace_end();

    /* The requested URL and wire-visible security metadata remain exact. */
    CHECK(trace_replay_rejects(
              budget, trace_directory, changed_url, &replayed_request));
    FetchRequest mismatch = replayed_request;
    mismatch.referer = changed_referer;
    CHECK(trace_replay_rejects(
              budget, trace_directory, url, &mismatch));
    mismatch = replayed_request;
    mismatch.initiator_url = "http://different.test/context?nonce=replayed";
    CHECK(trace_replay_rejects(
              budget, trace_directory, url, &mismatch));

    /* The authoritative count excludes stale records from an older capture. */
    FetchRequest stale_request = {
        .method = "GET", .allow_http_errors = true
    };
    CHECK(trace_replay_rejects(
              budget, trace_directory, stale_url, &stale_request));

    /* Retained clock-only corpora remain replayable through bounded
       contiguous discovery when completion/count metadata is absent. */
    CHECK(trace_rewrite_info_as_legacy(trace_directory)
          && fetch_trace_replay_begin(
                 trace_directory, error, sizeof(error)));
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_redirect(budget, url, &replayed_request, &result));
    fetch_result_destroy(&result);
    fetch_trace_end();
    return budget->current == baseline;
}

static bool test_delayed_top_level_trace_seed(
    Budget *budget, unsigned port, const char *trace_directory)
{
    size_t baseline = budget->current;
    BrowserSession session;
    CHECK(browser_session_init(&session, budget, 64 * 1024));
    char url[256], cookie_header[4096] = {0}, error[256] = {0};
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/accept-encoding", port);
    TilefinchRequestContext top_level = {
        .target_url = url, .initiator_url = url, .top_level_url = url,
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_DOCUMENT,
        .top_level_navigation = true, .user_activated = true
    };
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true,
        .send_low_client_hints = true,
        .accept = "text/html,*/*;q=0.8",
        .sec_fetch_dest = "document", .sec_fetch_mode = "navigate",
        .sec_fetch_site = "none", .sec_fetch_user = true,
        .upgrade_insecure_requests = true,
        .credentials = FETCH_CREDENTIALS_INCLUDE,
        .credential_origin = url, .initiator_url = url,
        .cookie_session = &session, .cookie_context = &top_level
    };
    CHECK(fetch_trace_capture_arm_top_level(
        trace_directory, 2, error, sizeof(error)));
    FetchResult result = {.budget = budget};

    /* Transport faults are rejected before either path consumes an armed
       ordinal. The later selected request must still become record 0000. */
    fetch_inject_failure_once(FETCH_INJECT_TLS);
    CHECK(!fetch_redirect(budget, url, &request, &result));
    fetch_result_destroy(&result);
    CHECK(!trace_file_exists(trace_directory, "trace.meta")
          && !trace_file_exists(trace_directory, "0000.meta"));

    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64 * 1024);
    CHECK(scheduler != NULL);
    fetch_inject_failure_once(FETCH_INJECT_TLS);
    CHECK(!fetch_scheduler_request(
              scheduler, url, &request, 64 * 1024, 5000, &result));
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    CHECK(!trace_file_exists(trace_directory, "trace.meta")
          && !trace_file_exists(trace_directory, "0000.meta"));

    /* A structurally valid but inconsistent request context must not be
       classified as the first top-level navigation. */
    char mismatched_url[256];
    snprintf(mismatched_url, sizeof(mismatched_url),
             "http://127.0.0.1:%u/final", port);
    TilefinchRequestContext mismatched = top_level;
    mismatched.target_url = mismatched_url;
    FetchRequest mismatched_request = request;
    mismatched_request.cookie_context = &mismatched;
    CHECK(!fetch_redirect(budget, url, &mismatched_request, &result));
    fetch_result_destroy(&result);
    CHECK(!trace_file_exists(trace_directory, "trace.meta")
          && !trace_file_exists(trace_directory, "0000.meta"));

    TilefinchRequestContext non_navigation = top_level;
    non_navigation.mode = TILEFINCH_REQUEST_MODE_CORS;
    non_navigation.destination = TILEFINCH_DESTINATION_FETCH;
    non_navigation.top_level_navigation = false;
    non_navigation.user_activated = false;
    FetchRequest non_navigation_request = request;
    non_navigation_request.cookie_context = &non_navigation;
    CHECK(fetch_redirect(budget, url, &non_navigation_request, &result));
    fetch_result_destroy(&result);
    CHECK(!trace_file_exists(trace_directory, "trace.meta")
          && !trace_file_exists(trace_directory, "0000.meta"));

    CHECK(fetch_redirect(budget, url, &request, &result));
    fetch_result_destroy(&result);
    CHECK(!trace_file_exists(trace_directory, "trace.meta")
          && !trace_file_exists(trace_directory, "0000.meta"));

    /* Preparatory subresource traffic is valid but does not consume the
       top-level ordinal. The scheduler and synchronous paths therefore share
       one counter without each counting this request. */
    TilefinchRequestContext subresource = top_level;
    subresource.mode = TILEFINCH_REQUEST_MODE_CORS;
    subresource.destination = TILEFINCH_DESTINATION_FETCH;
    subresource.top_level_navigation = false;
    subresource.user_activated = false;
    FetchRequest subresource_request = request;
    subresource_request.accept = "*/*";
    subresource_request.sec_fetch_dest = "empty";
    subresource_request.sec_fetch_mode = "cors";
    subresource_request.sec_fetch_site = "same-origin";
    subresource_request.sec_fetch_user = false;
    subresource_request.upgrade_insecure_requests = false;
    subresource_request.cookie_context = &subresource;
    scheduler = fetch_scheduler_create(budget, 1, 64 * 1024);
    CHECK(scheduler != NULL
          && fetch_scheduler_request(
                 scheduler, url, &subresource_request,
                 64 * 1024, 5000, &result));
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    CHECK(!trace_file_exists(trace_directory, "trace.meta")
          && !trace_file_exists(trace_directory, "0000.meta"));

    CHECK(browser_session_cookie_set_http(
              &session, url,
              "cf_clearance=trace-secret; Path=/; HttpOnly")
          && browser_session_cookie_set_http(
              &session, url, "empty=; Path=/")
          && browser_session_cookie_set_http(
              &session, url, "expired=stale-secret; Path=/"));
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        if (session.cookies[i].value != NULL
            && strcmp(session.cookies[i].name, "expired") == 0) {
            session.cookies[i].expires_at = 1;
        }
    }
    CHECK(browser_session_cookie_header_context(
        &session, &top_level, cookie_header, sizeof(cookie_header)));
    /* The selected navigation represents a reload/redirect after the first
       top-level request established the cookie jar. Its browser-derived
       source context is intentionally different from a fresh replay start. */
    FetchRequest resumed_request = request;
    resumed_request.referer = url;
    resumed_request.sec_fetch_site = "same-origin";
    resumed_request.sec_fetch_user = false;
    resumed_request.referrer_source = url;
    scheduler = fetch_scheduler_create(budget, 1, 64 * 1024);
    CHECK(scheduler != NULL
          && fetch_scheduler_request(
                 scheduler, url, &resumed_request,
                 64 * 1024, 5000, &result));
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    CHECK(trace_file_exists(trace_directory, "trace.meta")
          && trace_file_exists(trace_directory, "0000.meta")
          && trace_file_exists(trace_directory, "0000.body")
          && trace_info_contains(
                 trace_directory,
                 "replay-start=delayed-top-level-v1")
          && trace_info_contains(trace_directory, "cookie-seed-version=1")
          && trace_info_contains(trace_directory, "cookie-seed-count=2")
          && trace_info_contains(trace_directory,
                                 "cookie-seed-0-name=cf_clearance")
          && trace_info_contains(trace_directory,
                                 "cookie-seed-0-value-length=12")
          && trace_info_contains(trace_directory,
                                 "cookie-seed-1-name=empty")
          && trace_info_contains(trace_directory,
                                 "cookie-seed-1-value-length=0")
          && !trace_info_contains(trace_directory, "trace-secret")
          && !trace_info_contains(trace_directory, "stale-secret")
          && !trace_info_contains(trace_directory, "expired"));

    BrowserSession replay;
    CHECK(browser_session_init(&replay, budget, 64 * 1024)
          && fetch_trace_replay_begin(
                 trace_directory, error, sizeof(error))
          && fetch_trace_replay_seed_session(
                 &replay, error, sizeof(error))
          && fetch_trace_replay_seed_session(
                 &replay, error, sizeof(error)));
    top_level.target_url = url;
    request.cookie_session = &replay;
    CHECK(browser_session_cookie_header_context(
              &replay, &top_level, cookie_header, sizeof(cookie_header))
          && strcmp(cookie_header,
                    "cf_clearance=xxxxxxxxxxxx; empty=") == 0);
    request.cookie = cookie_header;
    scheduler = fetch_scheduler_create(budget, 1, 64 * 1024);
    result = (FetchResult) {.budget = budget};
    CHECK(scheduler != NULL
          && fetch_scheduler_request(
                 scheduler, url, &request, 64 * 1024, 5000, &result));
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    browser_session_destroy(&replay);

    /* A malformed duplicate field is rejected before importing anything. */
    CHECK(trace_append_duplicate_seed(trace_directory));
    BrowserSession rejected;
    CHECK(browser_session_init(&rejected, budget, 64 * 1024)
          && fetch_trace_replay_begin(
                 trace_directory, error, sizeof(error))
          && !fetch_trace_replay_seed_session(
                 &rejected, error, sizeof(error))
          && rejected.cookie_bytes == 0 && rejected.cookie_clock == 0);
    fetch_trace_end();
    browser_session_destroy(&rejected);

    /* Traces predating cookie-shape metadata remain a successful no-op and
       must not alter a caller's existing jar. */
    CHECK(trace_rewrite_info_as_legacy(trace_directory));
    BrowserSession legacy;
    CHECK(browser_session_init(&legacy, budget, 64 * 1024)
          && browser_session_cookie_set_http(
                 &legacy, url, "legacy=retained; Path=/")
          && fetch_trace_replay_begin(
                 trace_directory, error, sizeof(error))
          && fetch_trace_replay_seed_session(
                 &legacy, error, sizeof(error))
          && browser_session_cookie_header_context(
                 &legacy, &top_level, cookie_header, sizeof(cookie_header))
          && strcmp(cookie_header, "legacy=retained") == 0);
    fetch_trace_end();
    browser_session_destroy(&legacy);
    CHECK(trace_append_embedded_nul(trace_directory)
          && !fetch_trace_replay_begin(
                 trace_directory, error, sizeof(error)));
    fetch_trace_end();
    CHECK(fetch_trace_capture_arm_top_level(
              trace_directory, 1, error, sizeof(error))
          && !trace_file_exists(trace_directory, "trace.meta"));
    fetch_trace_end();
    browser_session_destroy(&session);
    return budget->current == baseline;
}

static bool test_response_cookie_trace_capacity(
    Budget *budget, unsigned port, const char *trace_directory)
{
    size_t baseline = budget->current;
    char url[256], error[256] = {0};
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/response-cookie-capacity", port);
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true
    };
    FetchResult captured = {.budget = budget};
    CHECK(fetch_trace_capture_begin(
              trace_directory, error, sizeof(error))
          && !trace_info_contains(trace_directory, "cookie-seed-")
          && fetch_redirect(budget, url, &request, &captured)
          && captured.set_cookie_count == FETCH_RESPONSE_COOKIE_CAPACITY);
    fetch_trace_end();
    fetch_result_destroy(&captured);

    FetchResult replayed = {.budget = budget};
    CHECK(fetch_trace_replay_begin(
              trace_directory, error, sizeof(error))
          && fetch_redirect(budget, url, &request, &replayed)
          && replayed.set_cookie_count == FETCH_RESPONSE_COOKIE_CAPACITY);
    for (size_t i = 0; i < FETCH_RESPONSE_COOKIE_CAPACITY; i++) {
        char expected[64];
        int written = snprintf(expected, sizeof(expected),
                               "cookie%zu=xxxxx; Path=/", i);
        CHECK(written > 0 && (size_t) written < sizeof(expected)
              && replayed.set_cookies[i] != NULL
              && strcmp(replayed.set_cookies[i], expected) == 0
              && strstr(fetch_set_cookie_url(&replayed, i, ""),
                        "/response-cookie-capacity") != NULL);
    }
    fetch_trace_end();
    fetch_result_destroy(&replayed);

    char meta_path[4096];
    int meta_written = snprintf(
        meta_path, sizeof(meta_path), "%s/0000.meta", trace_directory);
    static const char count_line[] = "set-cookie-count=16";
    CHECK(meta_written > 0 && (size_t) meta_written < sizeof(meta_path)
          && replace_trace_byte_at_offset(
                 meta_path, count_line, sizeof(count_line) - 2, '7')
          && trace_replay_rejects(
                 budget, trace_directory, url, &request)
          && replace_trace_byte_at_offset(
                 meta_path, "set-cookie-count=17",
                 sizeof(count_line) - 2, '6'));

    /* Legacy formats do not require response-URL attribution, but every
       declared value index must still exist. First prove the otherwise-valid
       v3 compatibility path, then create a duplicate index and missing slot
       which the old v6-only completeness check allowed through. */
    CHECK(trace_rewrite_record_version(
        trace_directory, 0, "11", "3"));
    FetchResult legacy = {.budget = budget};
    CHECK(fetch_trace_replay_begin(
        trace_directory, error, sizeof(error)));
    bool legacy_ok = fetch_redirect(budget, url, &request, &legacy);
    fetch_trace_end();
    CHECK(legacy_ok
          && legacy.set_cookie_count == FETCH_RESPONSE_COOKIE_CAPACITY);
    fetch_result_destroy(&legacy);

    static const char value_zero[] = "set-cookie-0=cookie0";
    static const char value_gap[] = "set-cookie-1=cookie0";
    const size_t cookie_index = sizeof("set-cookie-") - 1;
    CHECK(replace_trace_byte_at_offset(
              meta_path, value_zero, cookie_index, '1')
          && trace_replay_rejects(
                 budget, trace_directory, url, &request)
          && replace_trace_byte_at_offset(
                 meta_path, value_gap, cookie_index, '0'));

    legacy = (FetchResult) {.budget = budget};
    CHECK(fetch_trace_replay_begin(
        trace_directory, error, sizeof(error)));
    legacy_ok = fetch_redirect(budget, url, &request, &legacy);
    fetch_trace_end();
    CHECK(legacy_ok
          && legacy.set_cookie_count == FETCH_RESPONSE_COOKIE_CAPACITY);
    fetch_result_destroy(&legacy);
    CHECK(trace_rewrite_record_version(
        trace_directory, 0, "3", "11"));
    return budget->current == baseline;
}

static bool test_scheduled_cookie_trace_roundtrip(
    Budget *budget, unsigned port, const char *trace_directory)
{
    size_t baseline = budget->current;
    static const char cookie_header[] = "cf_clearance=trace-secret";
    BrowserSession session, empty_session;
    CHECK(browser_session_init(&session, budget, 64 * 1024)
          && browser_session_init(&empty_session, budget, 64 * 1024));
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/final", port);
    CHECK(browser_session_cookie_set_http(
        &session, url, "cf_clearance=trace-secret; Path=/"));
    TilefinchRequestContext context = {
        .target_url = url, .initiator_url = url, .top_level_url = url,
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_FETCH
    };
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true,
        .credentials = FETCH_CREDENTIALS_INCLUDE,
        .credential_origin = url, .initiator_url = url,
        .cookie_session = &session, .cookie_context = &context
    };
    char error[256] = {0};
    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64 * 1024);
    CHECK(scheduler != NULL
          && fetch_trace_capture_begin(
                 trace_directory, error, sizeof(error)));
    FetchResult result = {.budget = budget};
    CHECK(fetch_scheduler_request(
              scheduler, url, &request, 64 * 1024, 5000, &result)
          && strstr(result.data, cookie_header) != NULL);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();

    char cookie_bytes[80];
    int bytes_written = snprintf(
        cookie_bytes, sizeof(cookie_bytes), "request-cookie-bytes=%zu\n",
        strlen(cookie_header));
    CHECK(bytes_written > 0
          && (size_t) bytes_written < sizeof(cookie_bytes)
          && trace_meta_contains(trace_directory, cookie_bytes)
          && trace_meta_contains(
                 trace_directory, "request-has-cf-clearance=1\n"));

    scheduler = fetch_scheduler_create(budget, 1, 64 * 1024);
    CHECK(scheduler != NULL
          && fetch_trace_replay_begin(
                 trace_directory, error, sizeof(error)));
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_scheduler_request(
              scheduler, url, &request, 64 * 1024, 5000, &result)
          && strstr(result.data, cookie_header) != NULL);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();

    request.cookie_session = &empty_session;
    scheduler = fetch_scheduler_create(budget, 1, 64 * 1024);
    CHECK(scheduler != NULL
          && fetch_trace_replay_begin(
                 trace_directory, error, sizeof(error)));
    result = (FetchResult) {.budget = budget};
    CHECK(!fetch_scheduler_request(
              scheduler, url, &request, 64 * 1024, 5000, &result)
          && strstr(result.error, "does not match") != NULL);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    browser_session_destroy(&empty_session);
    browser_session_destroy(&session);
    return budget->current == baseline;
}

static bool test_redirect_trace_roundtrip(Budget *budget, unsigned port,
                                          const char *trace_directory)
{
    char url[256];
    char error[256] = {0};
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/same", port);
    FetchRequest request = {
        .method = "GET", .cookie = "secret=initial",
        .extra_headers =
            "Authorization: Bearer fixture\nX-Public: retained",
        .allow_http_errors = true,
        .send_low_client_hints = true,
        .referer = "http://example.test/source",
        .accept = "text/plain,*/*;q=0.1",
        .sec_fetch_dest = "empty",
        .sec_fetch_mode = "cors",
        .sec_fetch_site = "same-origin",
        .initiator_url = url,
        .referrer_source = "http://example.test/source",
        .referrer_policy = "strict-origin-when-cross-origin"
    };
    FetchResult captured = {.budget = budget};
    CHECK(fetch_trace_capture_begin(trace_directory, error, sizeof(error))
          && fetch_redirect(budget, url, &request, &captured)
          && captured.set_cookie_count == 2);
    fetch_trace_end();
    fetch_result_destroy(&captured);

    FetchResult replayed = {.budget = budget};
    CHECK(fetch_trace_replay_begin(trace_directory, error, sizeof(error))
          && fetch_redirect(budget, url, &request, &replayed)
          && replayed.set_cookie_count == 2
          && strcmp(replayed.set_cookies[0], "hop=xxxx; Path=/") == 0
          && strcmp(replayed.set_cookies[1],
                    "final_cookie=xxxx; Path=/") == 0
          && strstr(fetch_set_cookie_url(&replayed, 0, ""), "/same") != NULL
          && strstr(fetch_set_cookie_url(&replayed, 1, ""), "/final") != NULL);
    fetch_trace_end();
    fetch_result_destroy(&replayed);

    FetchRequest mismatch = request;
    mismatch.cookie = "cf_clearance=x";
    CHECK(strlen(mismatch.cookie) == strlen(request.cookie)
          && trace_replay_rejects(
              budget, trace_directory, url, &mismatch));

    mismatch = request;
    mismatch.extra_headers =
        "Authorization: Bearer fixture\nX-Privat: retained";
    CHECK(strlen(mismatch.extra_headers) == strlen(request.extra_headers)
          && trace_replay_rejects(
              budget, trace_directory, url, &mismatch));

    /* allow_http_errors is client-side error-tolerance policy, never
       serialized onto the wire, so flipping it must NOT strand the
       record (mastodon avatars and feed media were recorded through a
       preload phase that tagged the flag differently). */
    mismatch = request;
    mismatch.allow_http_errors = false;
    {
        char accept_error[256] = {0};
        FetchResult accepted = {.budget = budget};
        CHECK(fetch_trace_replay_begin(trace_directory, accept_error,
                                       sizeof(accept_error))
              && fetch_redirect(budget, url, &mismatch, &accepted));
        fetch_trace_end();
        fetch_result_destroy(&accepted);
    }

    /* Dropping a recorded client hint still rejects: the recorded wire
       request offered more than this replay request would. */
    mismatch = request;
    mismatch.send_low_client_hints = false;
    CHECK(trace_replay_rejects(
        budget, trace_directory, url, &mismatch));

    mismatch = request;
    mismatch.user_agent = "";
    CHECK(trace_replay_rejects(
        budget, trace_directory, url, &mismatch));

    char meta_path[4096];
    int meta_written = snprintf(
        meta_path, sizeof(meta_path), "%s/0000.meta", trace_directory);
    CHECK(meta_written > 0 && (size_t) meta_written < sizeof(meta_path)
          && replace_trace_byte(
              meta_path, "request-diagnostic-mobile-safari", 'x')
          && trace_replay_rejects(
              budget, trace_directory, url, &request)
          && replace_trace_byte(
              meta_path, "xequest-diagnostic-mobile-safari", 'r'));

    char body_path[4096];
    int written = snprintf(body_path, sizeof(body_path), "%s/0000.body",
                           trace_directory);
    CHECK(written > 0 && (size_t) written < sizeof(body_path));
    FILE *body = fopen(body_path, "r+b");
    CHECK(body != NULL);
    int first = fgetc(body);
    CHECK(first != EOF && fseek(body, 0, SEEK_SET) == 0
          && fputc(first ^ 1, body) != EOF && fclose(body) == 0);
    CHECK(trace_replay_rejects(budget, trace_directory, url, &request));

    StreamProbe corrupt_probe = {0};
    FetchStreamOptions stream = {
        .on_headers = stream_headers,
        .on_body = stream_body,
        .opaque = &corrupt_probe
    };
    FetchStreamMetrics metrics = {0};
    FetchResult corrupt_result = {.budget = budget};
    CHECK(fetch_trace_replay_begin(trace_directory, error, sizeof(error))
          && !fetch_request_stream_cancelable(
              budget, url, &request, 64 * 1024, 5000, NULL, NULL,
              &stream, &metrics, &corrupt_result)
          && corrupt_probe.header_calls == 0
          && corrupt_probe.length == 0);
    fetch_trace_end();
    fetch_result_destroy(&corrupt_result);

    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64 * 1024);
    CHECK(scheduler != NULL
          && fetch_trace_replay_begin(trace_directory, error, sizeof(error)));
    uint64_t id = fetch_scheduler_enqueue_stream(
        scheduler, url, &request, 64 * 1024, 5000, &stream);
    bool success = true, taken = false;
    FetchStreamMetrics scheduled_metrics = {0};
    corrupt_result = (FetchResult) {.budget = budget};
    for (size_t pump = 0; id != 0 && pump < 4 && !taken; pump++) {
        (void) fetch_scheduler_pump(scheduler, 1, 0);
        taken = fetch_scheduler_take_stream(
            scheduler, id, &success, &scheduled_metrics, &corrupt_result);
    }
    CHECK(id != 0 && taken && !success
          && corrupt_probe.header_calls == 0
          && corrupt_probe.length == 0);
    fetch_result_destroy(&corrupt_result);
    fetch_scheduler_destroy(scheduler);
    fetch_trace_end();
    return true;
}
