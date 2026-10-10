bool youtube_lite_feed_probe_authorization(BrowserSession *session,
    const TilefinchRequestContext *context, uint64_t seconds,
    char *output, size_t capacity)
{
    return lite_feed_authorization(session, context, seconds, output, capacity);
}

static bool lite_probe_boolean(const YoutubeLiteSpan *object,
                               const char *name, bool *value)
{
    YoutubeLiteSpan span;
    if (!lite_json_member(object, name, &span)) return false;
    size_t length = (size_t) (span.end - span.start);
    if (length == 4u && memcmp(span.start, "true", 4u) == 0) {
        *value = true;
        return true;
    }
    if (length == 5u && memcmp(span.start, "false", 5u) == 0) {
        *value = false;
        return true;
    }
    return false;
}

bool youtube_lite_feed_probe_classify(Budget *budget, const char *json,
                                    size_t length, YoutubeLiteFeedProbe *result)
{
    if (result == NULL) return false;
    result->api_json = false;
    result->api_error = false;
    result->api_login_known = false;
    result->api_logged_out = false;
    result->api_message = false;
    result->api_signin_required = false;
    result->result_count = 0;
    if (budget == NULL || json == NULL || length == 0
        || length > YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES) return false;
    YoutubeLiteSpan root, member, context;
    if (!lite_json_span(json, json + length, &root)
        || *root.start != '{') return false;
    const char *tail = root.end;
    while (tail < json + length && isspace((unsigned char) *tail)) tail++;
    if (tail != json + length) return false;
    result->api_json = true;
    result->api_error = lite_json_member(&root, "error", &member);
    if (lite_json_member(&root, "responseContext", &context)
        && lite_json_member(&context, "mainAppWebResponseContext", &member))
        result->api_login_known = lite_probe_boolean(
            &member, "loggedOut", &result->api_logged_out);
    /* MWEB also reports authentication through its bounded tracking-parameter
       schema. Classify only the fixed logged_in flag, never other metadata. */
    if (!result->api_login_known
        && lite_json_member(&root, "responseContext", &context)
        && lite_json_member(&context, "serviceTrackingParams", &member)) {
        const char *service_cursor = NULL;
        YoutubeLiteSpan service;
        for (unsigned i = 0; i < 64u
             && lite_array_next(&member, &service_cursor, &service); i++) {
            YoutubeLiteSpan parameters, parameter;
            const char *parameter_cursor = NULL;
            if (!lite_json_member(&service, "params", &parameters)) continue;
            for (unsigned j = 0; j < 64u
                 && lite_array_next(&parameters, &parameter_cursor, &parameter); j++) {
                char key[32], value[8];
                if (lite_member_string(&parameter, "key", key, sizeof(key), false)
                    && strcmp(key, "logged_in") == 0
                    && lite_member_string(&parameter, "value", value, sizeof(value), false)
                    && (strcmp(value, "0") == 0 || strcmp(value, "1") == 0)) {
                    result->api_login_known = true;
                    result->api_logged_out = value[0] == '0';
                    break;
                }
            }
            if (result->api_login_known) break;
        }
    }
    result->api_message = lite_json_key(&root, "messageRenderer", NULL, &member);
    result->api_signin_required = lite_json_key(&root, "signInEndpoint", NULL, &member);
    if (result->api_error) return true;
    YoutubeLiteVideo *videos = budget_calloc_category(
        budget, BUDGET_CATEGORY_RESOURCE, YOUTUBE_LITE_MAXIMUM_RESULTS,
        sizeof(*videos));
    if (videos == NULL) return false;
    result->result_count = lite_parse_videos(json, length, videos,
                                           YOUTUBE_LITE_MAXIMUM_RESULTS);
    lite_probe_wipe(videos, sizeof(*videos) * YOUTUBE_LITE_MAXIMUM_RESULTS);
    budget_free(budget, videos);
    return true;
}

bool youtube_lite_probe_feed(Budget *budget, BrowserSession *session,
                            bool subscriptions, YoutubeLiteFeedProbe *result)
{
    if (budget == NULL || session == NULL || result == NULL) return false;
    *result = (YoutubeLiteFeedProbe) {0};
    /* Service sign-in belongs to ordinary browser navigation, including its
       forms and scripts. A data check must not replay single-use handoff
       redirects or silently initiate another account transition. */
    const char *origin = "https://m.youtube.com";
    const char *bootstrap = "https://m.youtube.com/";
    TilefinchRequestContext context = lite_primary_context(bootstrap, bootstrap);
    FetchRequest transport = {.method = "GET", .accept = "text/html",
        .user_agent = YOUTUBE_LITE_MOBILE_UA, .allow_http_errors = true};
    FetchPreparedPageRequest prepared;
    FetchResult source = {0};
    bool okay = fetch_prepare_page_request_context(
        &context, bootstrap, "strict-origin-when-cross-origin", session,
        NULL, NULL, &transport, &prepared, NULL)
        && fetch_request_cancelable(budget, bootstrap,
            fetch_prepared_page_request(&prepared),
            YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES, 30000, NULL, NULL, &source);
    result->bootstrap_status = source.status_code;
    result->bootstrap_bytes = source.length;
    result->bootstrap_same_origin = source.effective_url[0] != '\0'
        && tilefinch_url_same_origin(source.effective_url, bootstrap);
    result->bootstrap_destination = source.effective_url[0] == '\0' ? 0u
        : result->bootstrap_same_origin ? 1u
        : tilefinch_url_same_origin(source.effective_url, "https://accounts.google.com") ? 2u
        : tilefinch_url_same_origin(source.effective_url, "https://support.google.com") ? 3u
        : tilefinch_url_same_origin(source.effective_url, "https://www.youtube.com") ? 4u : 5u;
    /* Never derive API parameters from an account/consent redirect. */
    if (!okay || source.status_code != 200
        || !result->bootstrap_same_origin || source.data == NULL) {
        lite_probe_release(&source);
        lite_probe_wipe(&prepared, sizeof(prepared));
        return false;
    }
    lite_accept_cookies(session, &context, &source, bootstrap);
    YoutubeLiteIdentity identity = {.version = YOUTUBE_LITE_IDENTITY_CACHE_VERSION};
    char client_name[32] = {0};
    for (size_t at = 0; at < source.length; at++) {
        if (source.data[at] == 'I') {
            if (identity.api_key[0] == '\0')
                (void) lite_json_key_string_at(source.data, source.length, at,
                    "INNERTUBE_API_KEY", identity.api_key, sizeof(identity.api_key));
            if (identity.client_version[0] == '\0')
                (void) lite_json_key_string_at(source.data, source.length, at,
                    "INNERTUBE_CONTEXT_CLIENT_VERSION", identity.client_version,
                    sizeof(identity.client_version));
            if (client_name[0] == '\0')
                (void) lite_json_key_string_at(source.data, source.length, at,
                    "INNERTUBE_CLIENT_NAME", client_name, sizeof(client_name));
        }
        if (source.data[at] == 'V' && identity.visitor[0] == '\0')
            (void) lite_json_key_string_at(source.data, source.length, at,
                "VISITOR_DATA", identity.visitor, sizeof(identity.visitor));
    }
    const char *login = lite_find_bytes(source.data, source.data + source.length,
                                      "\"LOGGED_IN\"");
    if (login != NULL) {
        const char *value = login + strlen("\"LOGGED_IN\"");
        const char *end = source.data + source.length;
        while (value < end && isspace((unsigned char) *value)) value++;
        if (value < end && *value++ == ':') {
            while (value < end && isspace((unsigned char) *value)) value++;
            if ((size_t) (end - value) >= 4u && memcmp(value, "true", 4u) == 0) {
                result->login_known = true;
                result->logged_in = true;
            } else if ((size_t) (end - value) >= 5u
                       && memcmp(value, "false", 5u) == 0) {
                result->login_known = true;
            }
        }
    }
    lite_probe_release(&source);
    char endpoint[512], body[4096], visitor[2048], headers[512], authorization[96];
    okay = lite_identity_valid(&identity) && strcmp(client_name, "MWEB") == 0
        && lite_json_quote(identity.visitor, visitor, sizeof(visitor));
    int endpoint_length = snprintf(endpoint, sizeof(endpoint),
        "%s/youtubei/v1/browse?key=%s&prettyPrint=false", origin, identity.api_key);
    int body_length = snprintf(body, sizeof(body),
        "{\"context\":{\"client\":{\"clientName\":\"MWEB\","
        "\"clientVersion\":\"%s\",\"hl\":\"en\",\"gl\":\"US\","
        "\"visitorData\":%s}},\"browseId\":\"%s\"}",
        identity.client_version, okay ? visitor : "\"\"",
        subscriptions ? "FEsubscriptions" : "FEwhat_to_watch");
    context = lite_supplemental_context(bootstrap, endpoint);
    context.initiator_url = bootstrap;
    result->authorization_present = result->logged_in
        && youtube_lite_feed_probe_authorization(session, &context,
            tilefinch_platform_wall_time_ns() / UINT64_C(1000000000),
            authorization, sizeof(authorization));
    int headers_length = snprintf(headers, sizeof(headers),
        "X-YouTube-Client-Name: 2\nX-YouTube-Client-Version: %s%s%s%s",
        identity.client_version,
        result->authorization_present ? "\nAuthorization: " : "",
        result->authorization_present ? authorization : "",
        result->authorization_present ? "\nX-Origin: https://m.youtube.com\nX-Goog-AuthUser: 0\nX-Youtube-Bootstrap-Logged-In: true" : "");
    okay = okay && endpoint_length > 0 && (size_t) endpoint_length < sizeof(endpoint)
        && body_length > 0 && (size_t) body_length < sizeof(body)
        && headers_length > 0 && (size_t) headers_length < sizeof(headers);
    transport = (FetchRequest) {.method = "POST", .body = body,
        .body_length = body_length > 0 ? (size_t) body_length : 0,
        .content_type = "application/json", .accept = "application/json",
        .extra_headers = headers, .allow_http_errors = true,
        .user_agent = YOUTUBE_LITE_MOBILE_UA,
        .redirect_same_origin_only = true};
    FetchResult response = {0};
    okay = okay && fetch_prepare_page_request_context(
        &context, bootstrap, "strict-origin-when-cross-origin", session,
        NULL, NULL, &transport, &prepared, NULL)
        && fetch_request_cancelable(budget, endpoint,
            fetch_prepared_page_request(&prepared),
            YOUTUBE_LITE_MAXIMUM_SOURCE_BYTES, 30000, NULL, NULL, &response);
    result->api_status = response.status_code;
    result->api_bytes = response.length;
    if (okay && response.data != NULL
        && tilefinch_url_same_origin(response.effective_url, bootstrap)) {
        lite_accept_cookies(session, &context, &response, endpoint);
        okay = youtube_lite_feed_probe_classify(budget, response.data,
                                               response.length, result);
    } else okay = false;
    lite_probe_release(&response);
    lite_probe_wipe(&prepared, sizeof(prepared));
    lite_probe_wipe(&identity, sizeof(identity));
    lite_probe_wipe(body, sizeof(body));
    lite_probe_wipe(visitor, sizeof(visitor));
    lite_probe_wipe(endpoint, sizeof(endpoint));
    lite_probe_wipe(headers, sizeof(headers));
    lite_probe_wipe(authorization, sizeof(authorization));
    return okay;
}
