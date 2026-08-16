static bool header_name_equals(const char *name, size_t length,
                               const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    return length == wanted_length
        && strncasecmp(name, wanted, wanted_length) == 0;
}

/* Extra headers are the page-controlled boundary. Browser metadata and
   transport-owned fields must enter through FetchRequest's structured
   members so a script cannot forge user activation, client hints, proxy
   credentials, framing, or connection state. Keep this predicate aligned
   with nativeHeaderBlock in js_runtime.c; the native check remains the
   authoritative fail-closed boundary for every caller and replay. */
static bool extra_header_name_forbidden(const char *name, size_t length)
{
    if (name == NULL || length == 0) return true;
    if ((length >= 4 && strncasecmp(name, "sec-", 4) == 0)
        || (length >= 6 && strncasecmp(name, "proxy-", 6) == 0)) {
        return true;
    }
    static const char *forbidden[] = {
        "accept", "accept-charset", "accept-encoding",
        "access-control-request-headers",
        "access-control-request-method",
        "access-control-request-private-network",
        "connection", "content-length", "content-type", "cookie",
        "cookie2", "date", "dnt", "expect", "host",
        "if-modified-since", "if-none-match", "keep-alive", "origin",
        "permissions-policy", "referer", "set-cookie", "set-cookie2",
        "te", "trailer", "transfer-encoding", "upgrade",
        "upgrade-insecure-requests", "user-agent", "via",
        /* Legacy client-hint spellings are also browser-owned. */
        "ua", "ua-mobile", "ua-platform", "ua-full-version",
        "ua-full-version-list", "ua-arch", "ua-bitness", "ua-model",
        "ua-platform-version",
        /* Fetch forbids method-override headers which can tunnel a forbidden
           CONNECT/TRACE/TRACK method through an intermediary. */
        "x-http-method", "x-http-method-override", "x-method-override"
    };
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        if (header_name_equals(name, length, forbidden[i])) return true;
    }
    return false;
}

static bool extra_headers_allowed(const char *serialized,
                                  bool cors_preflight)
{
    if (serialized == NULL || serialized[0] == '\0') return true;
    for (const char *at = serialized; *at != '\0';) {
        const char *end = strchr(at, '\n');
        if (end == NULL) end = at + strlen(at);
        const char *colon = memchr(at, ':', (size_t) (end - at));
        size_t name_length = colon == NULL ? 0 : (size_t) (colon - at);
        bool preflight_header = cors_preflight && colon != NULL
            && (header_name_equals(
                    at, name_length, "access-control-request-method")
                || header_name_equals(
                    at, name_length, "access-control-request-headers"));
        if (colon == NULL
            || (!preflight_header
                && extra_header_name_forbidden(at, name_length))) {
            return false;
        }
        at = *end == '\0' ? end : end + 1;
    }
    return true;
}

static bool header_name_character(unsigned char value)
{
    return value != '\0'
        && ((value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z')
        || (value >= '0' && value <= '9')
        || strchr("!#$%&'*+-.^_`|~", value) != NULL);
}

static bool header_value_valid(const char *value, size_t maximum_length)
{
    if (value == NULL) return true;
    size_t length = strlen(value);
    if (length > maximum_length) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char) value[i];
        if (byte < 0x20 || byte == 0x7f) return false;
    }
    return true;
}

static bool serialized_headers_valid(const char *serialized)
{
    if (serialized == NULL || serialized[0] == '\0') return true;
    size_t length = strlen(serialized);
    if (length > FETCH_EXTRA_HEADERS_MAX) return false;
    const char *at = serialized;
    while (*at != '\0') {
        const char *end = strchr(at, '\n');
        size_t line_length = end == NULL
            ? strlen(at) : (size_t) (end - at);
        if (line_length == 0 || line_length >= FETCH_EXTRA_HEADER_LINE_MAX) {
            return false;
        }
        const char *colon = memchr(at, ':', line_length);
        if (colon == NULL || colon == at) return false;
        for (const char *name = at; name < colon; name++) {
            if (!header_name_character((unsigned char) *name)) return false;
        }
        for (const char *value = colon + 1;
             value < at + line_length; value++) {
            unsigned char byte = (unsigned char) *value;
            if (byte < 0x20 || byte == 0x7f) return false;
        }
        if (end == NULL) break;
        if (end[1] == '\0') return false;
        at = end + 1;
    }
    return true;
}

static bool request_method_valid(const char *method)
{
    if (method == NULL) return true;
    size_t length = strlen(method);
    if (length == 0 || length > 16) return false;
    for (size_t i = 0; i < length; i++) {
        if (!header_name_character((unsigned char) method[i])) return false;
    }
    return true;
}

static bool fetch_destination_is_passive(const FetchRequest *request)
{
    const char *destination = request == NULL ? NULL
        : (request->page_context == NULL ? request->sec_fetch_dest
            : tilefinch_request_fetch_destination(request->page_context));
    return destination != NULL
        && (strcasecmp(destination, "image") == 0
            || strcasecmp(destination, "audio") == 0
            || strcasecmp(destination, "video") == 0
            || strcasecmp(destination, "media") == 0);
}

static bool fetch_request_is_top_level_navigation(const FetchRequest *request)
{
    return request != NULL && request->sec_fetch_dest != NULL
        && request->sec_fetch_mode != NULL
        && strcasecmp(request->sec_fetch_dest, "document") == 0
        && strcasecmp(request->sec_fetch_mode, "navigate") == 0;
}

static bool fetch_url_has_private_address_literal(const TilefinchUrl *url);

static bool fetch_url_names_local_network(const char *url)
{
    TilefinchUrl parsed;
    if (url == NULL || !tilefinch_url_parse(url, &parsed)) return false;
    if (fetch_url_has_private_address_literal(&parsed)) return true;
    const char *host = url + parsed.host_offset;
    size_t length = parsed.host_length;
    static const char localhost[] = "localhost";
    size_t local_length = sizeof(localhost) - 1u;
    static const char local_suffix[] = ".local";
    size_t suffix_length = sizeof(local_suffix) - 1u;
    return memchr(host, '.', length) == NULL
        || (length == local_length
            && strncasecmp(host, localhost, local_length) == 0)
        || (length > local_length
            && host[length - local_length - 1u] == '.'
            && strncasecmp(host + length - local_length,
                           localhost, local_length) == 0)
        || (length > suffix_length
            && strncasecmp(host + length - suffix_length,
                           local_suffix, suffix_length) == 0);
}

static bool fetch_request_private_network_protected(
    const FetchRequest *request, const char *initial_url)
{
    if (request == NULL) return false;
    if (request->block_private_network) return true;
    const TilefinchRequestContext *page = request->page_context;
    /* Only an explicit user-entered navigation whose requested address is
       visibly local gets the LAN escape hatch. A typed public URL remains
       protected, so a redirect or DNS rebind to RFC1918 cannot inherit broad
       top-level-navigation authority. */
    if (page != NULL && page->top_level_navigation && page->user_activated
        && fetch_url_names_local_network(page->target_url)) return false;
    if (page == NULL && fetch_request_is_top_level_navigation(request)
        && request->sec_fetch_user) {
        return !fetch_url_names_local_network(initial_url);
    }
    /* Native work has no page context. Local/private pages may keep using
       their own address space; a public page must not cross into it. */
    const char *initiator = request->page_context == NULL
        ? request->initiator_url : request->page_context->initiator_url;
    return request->page_context != NULL
        && !fetch_url_names_local_network(initiator);
}

static bool fetch_request_mixed_override(const FetchRequest *request)
{
    if (request == NULL || request->cookie_session == NULL) return false;
    const char *site = request->page_context != NULL
        && request->page_context->top_level_url != NULL
        ? request->page_context->top_level_url : request->initiator_url;
    return browser_session_mixed_content_site_allowed(
        request->cookie_session, site);
}

static bool fetch_url_has_private_address_literal(const TilefinchUrl *url)
{
    if (url == NULL || url->host_length == 0 || url->host_length >= 256u)
        return false;
    char host[256];
    size_t offset = url->host_offset;
    size_t length = url->host_length;
    if (url->ipv6_literal) {
        if (length < 3u) return false;
        offset++;
        length -= 2u;
    }
    memcpy(host, url->value + offset, length);
    host[length] = '\0';
    struct sockaddr_storage address;
    memset(&address, 0, sizeof(address));
    struct sockaddr_in *ipv4 = (struct sockaddr_in *) &address;
    if (inet_pton(AF_INET, host, &ipv4->sin_addr) == 1) {
        ipv4->sin_family = AF_INET;
        return fetch_private_sockaddr((const struct sockaddr *) &address);
    }
#ifdef AF_INET6
    unsigned char ipv6[16];
    if (inet_pton(AF_INET6, host, ipv6) == 1)
        return fetch_private_ipv6(ipv6);
#endif
    return false;
}

/* Apply HSTS and HTTPS-first before the request target is admitted. Passive
   mixed content is auto-upgraded once and never falls back to HTTP. Active
   mixed content remains HTTP here and is rejected below. */
static bool fetch_request_prepare_security_url_with_session(
    const FetchRequest *request, BrowserSession *security_session,
    const char *url,
    bool upgrade_initial_insecure_request,
    char output[TILEFINCH_URL_SERIALIZED_LIMIT])
{
    char upgraded[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (url == NULL || output == NULL) return false;
    if (!browser_session_hsts_upgrade_url(
            security_session,
            url, upgraded, sizeof(upgraded))) return false;
    TilefinchUrl target;
    if (!tilefinch_url_parse(upgraded, &target)) return false;
    bool ordinary_navigation = fetch_request_is_top_level_navigation(request)
        && !tilefinch_url_potentially_trustworthy(upgraded)
        /* Explicit LAN navigation is a supported escape hatch. Do not turn
           http://192.168.x.y into a request its appliance cannot answer. */
        && !fetch_url_names_local_network(upgraded);
    const char *initiator_url = request == NULL ? NULL
        : (request->page_context == NULL ? request->initiator_url
            : request->page_context->initiator_url);
    TilefinchUrl initiator;
    bool mixed_passive = initiator_url != NULL
        && tilefinch_url_parse(initiator_url, &initiator)
        && initiator.scheme == TILEFINCH_URL_SCHEME_HTTPS
        && target.scheme == TILEFINCH_URL_SCHEME_HTTP
        && fetch_destination_is_passive(request)
        && !fetch_request_mixed_override(request);
    if (target.scheme == TILEFINCH_URL_SCHEME_HTTP
        && upgrade_initial_insecure_request
        && (ordinary_navigation || mixed_passive)) {
        return tilefinch_url_upgrade_to_https(
            upgraded, output, TILEFINCH_URL_SERIALIZED_LIMIT);
    }
    return tilefinch_url_normalize(
        upgraded, output, TILEFINCH_URL_SERIALIZED_LIMIT);
}

static bool fetch_request_prepare_security_url(
    const FetchRequest *request, const char *url,
    bool upgrade_initial_insecure_request,
    char output[TILEFINCH_URL_SERIALIZED_LIMIT])
{
    return fetch_request_prepare_security_url_with_session(
        request,
        request == NULL ? NULL : (BrowserSession *) request->cookie_session,
        url, upgrade_initial_insecure_request, output);
}

/*
 * A secure document must not gain any HTTP channel through a script,
 * stylesheet, frame, font, media element, image, or fetch/XHR request.
 * Passive resources have already been upgraded above. The only exception is
 * a bounded, explicit top-level-site compatibility grant.
 *
 * This check belongs below every page resource loader so new callers cannot
 * accidentally omit it. It is also applied again to every explicit redirect
 * hop; libcurl never follows redirects on its own.
 */
static bool fetch_request_target_allowed(const FetchRequest *request,
                                         const char *target_url)
{
    const TilefinchRequestContext *page = request == NULL
        ? NULL : request->page_context;
    const char *initiator_url = page == NULL
        ? (request == NULL ? NULL : request->initiator_url)
        : page->initiator_url;
    if (target_url == NULL) return false;
    if (page != NULL && request->content_security_policy != NULL
        && !tilefinch_csp_allows_request(
               request->content_security_policy, page->destination,
               target_url)) return false;
    if (initiator_url == NULL) return true;
    TilefinchUrl initiator;
    TilefinchUrl target;
    if (!tilefinch_url_parse(initiator_url, &initiator)
        || !tilefinch_url_parse(target_url, &target)) return false;
    if (initiator.scheme != TILEFINCH_URL_SCHEME_HTTPS
        || target.scheme != TILEFINCH_URL_SCHEME_HTTP
        || tilefinch_url_potentially_trustworthy(target_url)) {
        return true;
    }
    return fetch_request_mixed_override(request);
}

static ContentBlocker *fetch_request_content_blocker(
    const FetchRequest *request)
{
    if (request == NULL) return NULL;
    if (request->content_blocker != NULL) return request->content_blocker;
    return request->cookie_session == NULL
        ? NULL : request->cookie_session->content_blocker;
}

static bool fetch_request_content_blocked(const FetchRequest *request,
                                          const char *target_url)
{
    ContentBlocker *blocker = fetch_request_content_blocker(request);
    const TilefinchRequestContext *page = request == NULL
        ? NULL : request->page_context;
    return blocker != NULL && content_blocker_should_block(
        blocker, target_url,
        page == NULL ? request->initiator_url : page->initiator_url,
        page == NULL ? request->sec_fetch_dest
                     : tilefinch_request_fetch_destination(page),
        page == NULL ? request->sec_fetch_mode
                     : tilefinch_request_fetch_mode(page));
}

static bool page_request_context_matches(
    const FetchRequest *request, const TilefinchRequestFacts *known_facts)
{
    if (request == NULL) return true;
    if ((request->page_context == NULL)
            != (request->prepared_page_version == 0)
        || (request->page_context != NULL
            && request->prepared_page_version
                   != FETCH_PREPARED_PAGE_REQUEST_VERSION)) return false;
    if (request->page_context == NULL) return true;
    const TilefinchRequestContext *context = request->page_context;
    TilefinchRequestFacts analyzed;
    const TilefinchRequestFacts *facts = known_facts;
    if (facts == NULL || facts->source != context || !facts->valid) {
        if (!tilefinch_request_context_analyze(context, &analyzed)) {
            return false;
        }
        facts = &analyzed;
    }
    const char *method = request->method == NULL ? "GET" : request->method;
    const char *context_method = context->method == NULL
        ? "GET" : context->method;
    FetchCredentialPolicy credentials =
        context->credentials == TILEFINCH_CREDENTIALS_OMIT
            ? FETCH_CREDENTIALS_OMIT
            : (context->credentials == TILEFINCH_CREDENTIALS_SAME_ORIGIN
                   ? FETCH_CREDENTIALS_SAME_ORIGIN
                   : FETCH_CREDENTIALS_INCLUDE);
    if (strcasecmp(method, context_method) != 0
        || request->credentials != credentials
        || ((request->initiator_url == NULL)
            != (context->initiator_url == NULL))
        || (request->initiator_url != NULL
            && strcmp(request->initiator_url,
                      context->initiator_url) != 0)
        || request->sec_fetch_dest == NULL
        || strcasecmp(request->sec_fetch_dest,
                      tilefinch_request_fetch_destination(context)) != 0
        || request->sec_fetch_mode == NULL
        || strcasecmp(request->sec_fetch_mode,
                      tilefinch_request_fetch_mode(context)) != 0
        || request->sec_fetch_site == NULL
        || strcasecmp(request->sec_fetch_site,
                      tilefinch_request_facts_fetch_site(facts)) != 0) {
        return false;
    }
    if (context->initiator_opaque
        && request->credentials == FETCH_CREDENTIALS_SAME_ORIGIN) {
        return false;
    }
    if (context->mode == TILEFINCH_REQUEST_MODE_SAME_ORIGIN
        && !facts->same_origin) return false;
    bool cors_mode = context->mode == TILEFINCH_REQUEST_MODE_CORS;
    if (cors_mode != (request->enforce_cors
                      || request->cors_response_check_deferred)
        || (request->enforce_cors
            && request->cors_response_check_deferred)) return false;
    if (request->cookie_context != NULL) {
        const TilefinchRequestContext *cookie = request->cookie_context;
        if (strcmp(cookie->target_url, context->target_url) != 0
            || ((cookie->initiator_url == NULL) !=
                (context->initiator_url == NULL))
            || (cookie->initiator_url != NULL
                && strcmp(cookie->initiator_url,
                          context->initiator_url) != 0)
            || cookie->mode != context->mode
            || cookie->credentials != context->credentials
            || cookie->destination != context->destination
            || cookie->initiator_opaque != context->initiator_opaque) {
            return false;
        }
    }
    return true;
}

static bool page_request_target_matches(const FetchRequest *request,
                                        const char *target_url)
{
    if (request == NULL || request->page_context == NULL) return true;
    char expected[TILEFINCH_URL_SERIALIZED_LIMIT];
    char actual[TILEFINCH_URL_SERIALIZED_LIMIT];
    return request->page_context->target_url != NULL
        && tilefinch_url_request_key(
               request->page_context->target_url,
               expected, sizeof(expected))
        && tilefinch_url_request_key(target_url, actual, sizeof(actual))
        && strcmp(expected, actual) == 0;
}

static bool cors_preflight_header_list_valid(const char *value,
                                             size_t length)
{
    const char *at = value;
    const char *end = value + length;
    bool saw_name = false;
    while (at < end) {
        while (at < end && (*at == ' ' || *at == '\t')) at++;
        const char *start = at;
        while (at < end && header_name_character((unsigned char) *at)) at++;
        if (at == start) return false;
        saw_name = true;
        while (at < end && (*at == ' ' || *at == '\t')) at++;
        if (at == end) break;
        if (*at++ != ',') return false;
        while (at < end && (*at == ' ' || *at == '\t')) at++;
        if (at == end) return false;
    }
    return saw_name;
}

static bool cors_preflight_shape_valid(const FetchRequest *request)
{
    if (request == NULL || !request->cors_preflight) return true;
    const char *method = request->method == NULL ? "GET" : request->method;
    if (strcasecmp(method, "OPTIONS") != 0
        || request->body != NULL || request->body_length != 0
        || request->content_type != NULL || request->cookie != NULL
        || request->origin == NULL || request->origin[0] == '\0'
        || request->extra_headers == NULL
        || request->credentials != FETCH_CREDENTIALS_SAME_ORIGIN
        || !request->enforce_cors
        || !request->redirect_same_origin_only) return false;
    size_t method_fields = 0, header_fields = 0;
    for (const char *at = request->extra_headers; *at != '\0';) {
        const char *end = strchr(at, '\n');
        if (end == NULL) end = at + strlen(at);
        const char *colon = memchr(at, ':', (size_t) (end - at));
        if (colon == NULL) return false;
        const char *value = colon + 1;
        const char *limit = end;
        while (value < limit && (*value == ' ' || *value == '\t')) value++;
        while (limit > value
               && (limit[-1] == ' ' || limit[-1] == '\t')) limit--;
        size_t name_length = (size_t) (colon - at);
        size_t value_length = (size_t) (limit - value);
        if (header_name_equals(
                at, name_length, "access-control-request-method")) {
            char requested[17];
            if (++method_fields != 1 || value_length == 0
                || value_length >= sizeof(requested)) return false;
            memcpy(requested, value, value_length);
            requested[value_length] = '\0';
            if (!request_method_valid(requested)
                || strcasecmp(requested, "CONNECT") == 0
                || strcasecmp(requested, "TRACE") == 0
                || strcasecmp(requested, "TRACK") == 0) return false;
        } else if (header_name_equals(
                       at, name_length,
                       "access-control-request-headers")) {
            if (++header_fields != 1
                || !cors_preflight_header_list_valid(
                       value, value_length)) return false;
        } else {
            return false;
        }
        at = *end == '\0' ? end : end + 1;
    }
    return method_fields == 1;
}

static bool client_hint_token_list_valid(const char *serialized)
{
    if (serialized == NULL || serialized[0] == '\0'
        || strlen(serialized) > 1023) return false;
    const char *at = serialized;
    bool saw_token = false;
    while (*at != '\0') {
        while (*at == ' ' || *at == '\t') at++;
        const char *start = at;
        while (*at != '\0'
               && header_name_character((unsigned char) *at)) at++;
        if (at == start) return false;
        saw_token = true;
        while (*at == ' ' || *at == '\t') at++;
        if (*at == '\0') break;
        if (*at++ != ',') return false;
        while (*at == ' ' || *at == '\t') at++;
        if (*at == '\0') return false;
    }
    return saw_token;
}

static bool fetch_request_validate_with_facts(
    const FetchRequest *request, const TilefinchRequestFacts *known_facts,
    FetchRequestValidationError *error)
{
    if (error != NULL) *error = FETCH_REQUEST_VALIDATION_OK;
    if (request == NULL) return true;
#define REQUEST_INVALID(kind) do {                                          \
    if (error != NULL) *error = (kind);                                     \
    return false;                                                           \
} while (0)
    if (!request_method_valid(request->method)) {
        REQUEST_INVALID(FETCH_REQUEST_VALIDATION_METHOD);
    }
    const char *method = request->method == NULL ? "GET" : request->method;
    /* Fetch forbids these methods before any same-origin/CORS decision.
       In particular, TRACE must never receive or reflect HttpOnly cookies. */
    if (strcasecmp(method, "CONNECT") == 0
        || strcasecmp(method, "TRACE") == 0
        || strcasecmp(method, "TRACK") == 0) {
        REQUEST_INVALID(FETCH_REQUEST_VALIDATION_METHOD);
    }
    if ((request->body_length != 0 && request->body == NULL)
        || request->body_length > FETCH_REQUEST_BODY_MAX) {
        REQUEST_INVALID(FETCH_REQUEST_VALIDATION_BODY);
    }
    if ((request->body != NULL || request->body_length != 0)
        && (strcasecmp(method, "GET") == 0
            || strcasecmp(method, "HEAD") == 0)) {
        REQUEST_INVALID(FETCH_REQUEST_VALIDATION_BODY);
    }
    if ((unsigned) request->credentials
            > (unsigned) FETCH_CREDENTIALS_SAME_ORIGIN
        || (request->credentials == FETCH_CREDENTIALS_SAME_ORIGIN
            && (request->credential_origin == NULL
                || !tilefinch_url_same_origin(request->credential_origin,
                                           request->credential_origin)))) {
        REQUEST_INVALID(FETCH_REQUEST_VALIDATION_CREDENTIALS);
    }
    if (request->send_client_hints) {
        char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
        if (!client_hint_token_list_valid(request->client_hint_tokens)
            || request->client_hint_origin == NULL
            || !tilefinch_url_origin(request->client_hint_origin, origin,
                                  sizeof(origin))
            || strcmp(origin, request->client_hint_origin) != 0) {
            REQUEST_INVALID(FETCH_REQUEST_VALIDATION_CONTEXT);
        }
    } else if (request->client_hint_tokens != NULL
               || request->client_hint_origin != NULL) {
        REQUEST_INVALID(FETCH_REQUEST_VALIDATION_CONTEXT);
    }
    bool cookie_context_valid = request->cookie_context == NULL;
    if (!cookie_context_valid && known_facts != NULL
        && known_facts->source == request->cookie_context
        && known_facts->valid) {
        cookie_context_valid = true;
    } else if (!cookie_context_valid) {
        cookie_context_valid =
            tilefinch_request_context_valid(request->cookie_context);
    }
    if ((request->cookie_session == NULL)
            != (request->cookie_context == NULL)
        || !cookie_context_valid
        || !page_request_context_matches(request, known_facts)) {
        REQUEST_INVALID(FETCH_REQUEST_VALIDATION_CONTEXT);
    }
    static const size_t limits[] = {
        255, 4095, 191, 127, 4095, 2047, 255, 63, 63, 63, 511, 63
    };
    const char *values[] = {
        request->content_type, request->cookie, request->if_none_match,
        request->if_modified_since, request->referer, request->origin,
        request->accept, request->sec_fetch_dest, request->sec_fetch_mode,
        request->sec_fetch_site, request->user_agent,
        request->referrer_policy
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (!header_value_valid(values[i], limits[i])) {
            REQUEST_INVALID(FETCH_REQUEST_VALIDATION_HEADER_VALUE);
        }
    }
    if ((request->sec_fetch_dest != NULL
         && request->sec_fetch_dest[0] == '\0')
        || (request->sec_fetch_mode != NULL
            && request->sec_fetch_mode[0] == '\0')
        || (request->sec_fetch_site != NULL
            && request->sec_fetch_site[0] == '\0')) {
        REQUEST_INVALID(FETCH_REQUEST_VALIDATION_HEADER_VALUE);
    }
    if (!serialized_headers_valid(request->extra_headers)) {
        REQUEST_INVALID(FETCH_REQUEST_VALIDATION_EXTRA_HEADERS);
    }
    if (!extra_headers_allowed(
            request->extra_headers, request->cors_preflight)
        || !cors_preflight_shape_valid(request)) {
        REQUEST_INVALID(FETCH_REQUEST_VALIDATION_EXTRA_HEADERS);
    }
    const char *contexts[] = {
        request->credential_origin, request->initiator_url,
        request->referrer_source, request->client_hint_origin
    };
    for (size_t i = 0; i < sizeof(contexts) / sizeof(contexts[0]); i++) {
        TilefinchUrl parsed;
        if (contexts[i] != NULL
            && (!header_value_valid(
                    contexts[i], TILEFINCH_URL_SERIALIZED_LIMIT - 1)
                || !tilefinch_url_parse(contexts[i], &parsed))) {
            REQUEST_INVALID(FETCH_REQUEST_VALIDATION_CONTEXT);
        }
    }
    if (request->enforce_cors) {
        char initiator_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
        bool opaque_initiator = request->page_context != NULL
            && request->page_context->initiator_opaque;
        if (request->initiator_url == NULL
            || request->origin == NULL || request->origin[0] == '\0'
            || (opaque_initiator
                ? strcmp(request->origin, "null") != 0
                : (!tilefinch_url_origin(
                       request->initiator_url, initiator_origin,
                       sizeof(initiator_origin))
                   || strcmp(initiator_origin, request->origin) != 0))
            || (request->credentials == FETCH_CREDENTIALS_SAME_ORIGIN
                && (opaque_initiator
                    || !tilefinch_url_same_origin(
                           request->credential_origin,
                           request->initiator_url)))) {
            REQUEST_INVALID(FETCH_REQUEST_VALIDATION_CONTEXT);
        }
    }
    if (request->cors_cached_response_validated
        && (!request->enforce_cors
            || (strcasecmp(method, "GET") != 0
                && strcasecmp(method, "HEAD") != 0)
            || ((request->if_none_match == NULL
                 || request->if_none_match[0] == '\0')
                && (request->if_modified_since == NULL
                    || request->if_modified_since[0] == '\0')))) {
        REQUEST_INVALID(FETCH_REQUEST_VALIDATION_CONTEXT);
    }
#undef REQUEST_INVALID
    return true;
}

bool fetch_request_validate(
    const FetchRequest *request, FetchRequestValidationError *error)
{
    return fetch_request_validate_with_facts(request, NULL, error);
}

#define FETCH_BLOCKED_ORIGIN_LIMIT 64
#define FETCH_BLOCKED_ORIGIN_HOST_LIMIT 128
static char fetch_blocked_origins[FETCH_BLOCKED_ORIGIN_LIMIT]
                                 [FETCH_BLOCKED_ORIGIN_HOST_LIMIT];
static size_t fetch_blocked_origin_count;

void fetch_set_blocked_origins(const char *const *hosts, size_t count)
{
    fetch_blocked_origin_count = 0;
    for (size_t i = 0; hosts != NULL && i < count
         && fetch_blocked_origin_count < FETCH_BLOCKED_ORIGIN_LIMIT; i++) {
        if (hosts[i] == NULL
            || strlen(hosts[i]) >= FETCH_BLOCKED_ORIGIN_HOST_LIMIT) continue;
        snprintf(fetch_blocked_origins[fetch_blocked_origin_count],
                 FETCH_BLOCKED_ORIGIN_HOST_LIMIT, "%s", hosts[i]);
        fetch_blocked_origin_count++;
    }
}

bool fetch_origin_blocked(const char *url)
{
    if (fetch_blocked_origin_count == 0 || url == NULL) return false;
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (!tilefinch_url_origin(url, origin, sizeof(origin))) return false;
    const char *host = strstr(origin, "://");
    host = host == NULL ? origin : host + 3;
    size_t host_length = strcspn(host, ":");
    for (size_t i = 0; i < fetch_blocked_origin_count; i++) {
        const char *entry = fetch_blocked_origins[i];
        size_t entry_length = strlen(entry);
        if (entry_length > host_length) continue;
        const char *tail = host + host_length - entry_length;
        if (strncasecmp(tail, entry, entry_length) != 0) continue;
        if (entry_length == host_length || tail[-1] == '.') return true;
    }
    return false;
}
