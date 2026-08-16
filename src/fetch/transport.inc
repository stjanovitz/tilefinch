static bool hop_replace(Budget *budget, char **target, const char *value,
                        size_t limit)
{
    char *copy = hop_copy(budget, value, limit);
    if (value != NULL && value[0] != '\0' && copy == NULL) return false;
    budget_free(budget, *target);
    *target = copy;
    return true;
}

static void fetch_hop_destroy(FetchHopState *hop)
{
    if (hop == NULL) return;
    Budget *budget = hop->budget;
    budget_free(budget, hop->fetch_site);
    budget_free(budget, hop->cookie_top_level);
    budget_free(budget, hop->cookie_initiator);
    browser_cookie_overlay_destroy(hop->cookie_overlay);
    budget_free(budget, hop->origin);
    budget_free(budget, hop->referrer_source);
    budget_free(budget, hop->referer);
    budget_free(budget, hop->extra_headers);
    budget_free(budget, hop->method);
    budget_free(budget, hop->initiator);
    budget_free(budget, hop->logical_url);
    budget_free(budget, hop->url);
    budget_free(budget, hop);
}

static bool fetch_github_release_url_allowed(const char *value);

static bool fetch_method_omits_same_origin_origin(const char *method)
{
    return method != NULL
        && (strcasecmp(method, "GET") == 0
            || strcasecmp(method, "HEAD") == 0);
}

static FetchHopState *fetch_hop_create(Budget *budget, const char *url,
                                       const char *logical_url,
                                       const FetchRequest *request,
                                       long timeout_ms)
{
    if (request != NULL
        && request->redirect_policy == FETCH_REDIRECT_GITHUB_RELEASE
        && !fetch_github_release_url_allowed(url)) return NULL;
    if (!fetch_request_target_allowed(request, url)) return NULL;
    if (request != NULL
        && request->redirect_url_validator != NULL
        && !request->redirect_url_validator(url)) return NULL;
    FetchHopState *hop = budget_calloc(budget, 1, sizeof(*hop));
    if (hop == NULL) return NULL;
    hop->budget = budget;
    hop->base = request;
    hop->security_session = request == NULL ? NULL
        : (BrowserSession *) request->cookie_session;
    const char *method = request == NULL || request->method == NULL
        ? "GET" : request->method;
    const char *initiator = request != NULL
        && request->initiator_url != NULL
        ? request->initiator_url : logical_url;
    const char *wire_origin = request == NULL ? NULL : request->origin;
    if (request != NULL && request->enforce_cors
        && fetch_method_omits_same_origin_origin(method)
        && tilefinch_url_same_origin(initiator, url)) {
        /* request->origin remains the validated CORS security context. A safe
           same-origin hop omits the wire header until a redirect crosses the
           origin boundary. */
        wire_origin = NULL;
    }
    hop->url = hop_copy(budget, url, 4095);
    hop->logical_url = hop_normalized_url(budget, logical_url);
    hop->initiator = hop_copy(budget, initiator, 4095);
    hop->method = hop_copy(budget, method, 16);
    hop->extra_headers = hop_copy(
        budget, request == NULL ? NULL : request->extra_headers,
        FETCH_EXTRA_HEADERS_MAX);
    hop->referer = hop_copy(
        budget, request == NULL ? NULL : request->referer, 4095);
    const char *referrer_source = request == NULL ? NULL
        : (request->referrer_source != NULL
              ? request->referrer_source : request->referer);
    hop->referrer_source = hop_copy(budget, referrer_source, 4095);
    select_referrer_policy(
        request == NULL ? NULL : request->referrer_policy,
        hop->referrer_policy);
    hop->origin = hop_copy(
        budget, wire_origin, 2047);
    hop->fetch_site = hop_copy(
        budget, request == NULL ? NULL : request->sec_fetch_site, 63);
    if (hop->url == NULL || hop->logical_url == NULL
        || hop->initiator == NULL || hop->method == NULL
        || (request != NULL && request->extra_headers != NULL
            && request->extra_headers[0] != '\0'
            && hop->extra_headers == NULL)
        || (request != NULL && request->referer != NULL
            && request->referer[0] != '\0' && hop->referer == NULL)
        || (referrer_source != NULL && referrer_source[0] != '\0'
            && hop->referrer_source == NULL)
        || (wire_origin != NULL && wire_origin[0] != '\0'
            && hop->origin == NULL)
        || (request != NULL && request->sec_fetch_site != NULL
            && request->sec_fetch_site[0] != '\0'
            && hop->fetch_site == NULL)) {
        fetch_hop_destroy(hop);
        return NULL;
    }
    if (request != NULL && request->cookie_session != NULL) {
        const TilefinchRequestContext *context = request->cookie_context;
        char target[TILEFINCH_URL_SERIALIZED_LIMIT];
        TilefinchCredentialsMode expected_credentials =
            request->credentials == FETCH_CREDENTIALS_OMIT
                ? TILEFINCH_CREDENTIALS_OMIT
                : (request->credentials == FETCH_CREDENTIALS_SAME_ORIGIN
                      ? TILEFINCH_CREDENTIALS_SAME_ORIGIN
                      : TILEFINCH_CREDENTIALS_INCLUDE);
        char secured_target[TILEFINCH_URL_SERIALIZED_LIMIT];
        hop->cookie_overlay = browser_session_cookie_overlay_create(
            budget, request->cookie_session);
        hop->cookie_initiator = hop_copy(
            budget, context->initiator_url, 2047);
        hop->cookie_top_level = hop_copy(
            budget, context->top_level_url, 2047);
        if (hop->cookie_overlay == NULL
            || !fetch_request_prepare_security_url(
                   request, context->target_url, true, secured_target)
            || !tilefinch_url_request_key(
                   secured_target, target, sizeof(target))
            || strcmp(target, url) != 0
            || strcasecmp(
                   context->method == NULL ? "GET" : context->method,
                   method) != 0
            || context->credentials != expected_credentials
            || (context->initiator_url != NULL
                && hop->cookie_initiator == NULL)
            || (context->top_level_url != NULL
                && hop->cookie_top_level == NULL)) {
            fetch_hop_destroy(hop);
            return NULL;
        }
        hop->cookie_mode = context->mode;
        hop->cookie_credentials = context->credentials;
        hop->cookie_destination = context->destination;
        hop->cookie_top_level_navigation = context->top_level_navigation;
        hop->cookie_user_activated = context->user_activated;
        hop->cookie_initiator_opaque = context->initiator_opaque;
    }
    hop->body_enabled = true;
    hop->fetch_site_none = request != NULL
        && request->sec_fetch_site != NULL
        && strcasecmp(request->sec_fetch_site, "none") == 0;
    hop->credentials_tainted = request != NULL
        && request->credentials == FETCH_CREDENTIALS_SAME_ORIGIN
        && !tilefinch_url_same_origin(request->credential_origin, url);
    uint64_t now = tilefinch_platform_monotonic_time_ns();
    uint64_t duration = (uint64_t) timeout_ms * UINT64_C(1000000);
    hop->deadline_ns = duration > UINT64_MAX - now ? UINT64_MAX
                                                   : now + duration;
    return hop;
}

static TilefinchRequestContext fetch_hop_cookie_context(
    const FetchHopState *hop, const char *target_url)
{
    return (TilefinchRequestContext) {
        .target_url = target_url,
        .initiator_url = hop->cookie_initiator,
        .top_level_url = hop->cookie_top_level,
        .method = hop->method,
        .mode = hop->cookie_mode,
        .credentials = hop->cookie_credentials,
        .destination = hop->cookie_destination,
        .initiator_opaque = hop->cookie_initiator_opaque,
        .top_level_navigation = hop->cookie_top_level_navigation,
        .user_activated = hop->cookie_user_activated
    };
}

static bool fetch_hop_configure_cookie(CURL *easy, FetchHopState *hop,
                                       const FetchRequest *request,
                                       bool initial_hop)
{
    /* `easy` is retained in the signature because the ordinary curl path
       configures the cookie snapshot beside the handle. The PSP background
       scheduler materializes the same header before handing a hop to its
       worker, and therefore deliberately passes NULL here. */
    (void) easy;
    if (hop == NULL) return false;
    hop->cookie_header[0] = '\0';
    if (request != NULL && request->credentials != FETCH_CREDENTIALS_OMIT
        && !(request->credentials == FETCH_CREDENTIALS_SAME_ORIGIN
             && hop->credentials_tainted)) {
        if (hop->cookie_overlay != NULL) {
            TilefinchRequestContext context = fetch_hop_cookie_context(
                hop, hop->url);
            if (!browser_cookie_overlay_header_context(
                    hop->cookie_overlay, &context,
                    hop->cookie_header, sizeof(hop->cookie_header))) {
                return false;
            }
        } else if (initial_hop && request->cookie != NULL) {
            size_t length = strlen(request->cookie);
            if (length >= sizeof(hop->cookie_header)) return false;
            memcpy(hop->cookie_header, request->cookie, length + 1);
        }
    }
    /* Never enable libcurl's cookie engine. The owned PSP transport compiles
       it out entirely, so even CURLOPT_COOKIE=NULL is an unsupported option.
       The literal value is appended to the explicit hop header list below;
       Set-Cookie fields stay in FetchResult and the private overlay until
       caller commit. */
    return true;
}

static void fetch_hop_stage_response_cookies(
    FetchHopState *hop, const FetchResult *result, size_t first)
{
    if (hop == NULL || hop->cookie_overlay == NULL || result == NULL) return;
    for (size_t i = first; i < result->set_cookie_count; i++) {
        const char *url = fetch_set_cookie_url(result, i, hop->url);
        TilefinchRequestContext context = fetch_hop_cookie_context(hop, url);
        (void) browser_cookie_overlay_set_http_context(
            hop->cookie_overlay, &context, result->set_cookies[i]);
    }
}

static bool hop_header_name(const char *line, size_t length,
                            const char *wanted)
{
    const char *colon = memchr(line, ':', length);
    size_t wanted_length = strlen(wanted);
    return colon != NULL && (size_t) (colon - line) == wanted_length
        && strncasecmp(line, wanted, wanted_length) == 0;
}

static bool fetch_hop_filter_headers(FetchHopState *hop, bool cross_origin,
                                     bool body_rewritten)
{
    if (hop->extra_headers == NULL) return true;
    size_t length = strlen(hop->extra_headers);
    char *filtered = budget_malloc(hop->budget, length + 1);
    if (filtered == NULL) return false;
    size_t used = 0;
    for (const char *at = hop->extra_headers; *at != '\0';) {
        const char *end = strchr(at, '\n');
        if (end == NULL) end = at + strlen(at);
        size_t line_length = (size_t) (end - at);
        bool drop = hop_header_name(at, line_length, "cookie")
            || hop_header_name(at, line_length, "host")
            || hop_header_name(at, line_length, "origin")
            || hop_header_name(at, line_length, "referer")
            || (line_length >= 10
                && strncasecmp(at, "sec-fetch-", 10) == 0)
            || (cross_origin
                && (hop_header_name(at, line_length, "authorization")
                    || hop_header_name(at, line_length,
                                       "proxy-authorization")))
            || (body_rewritten
                && (hop_header_name(at, line_length, "content-length")
                    || hop_header_name(at, line_length, "content-type")
                    || hop_header_name(at, line_length,
                                       "transfer-encoding")));
        if (!drop && line_length != 0) {
            memcpy(filtered + used, at, line_length);
            used += line_length;
            filtered[used++] = '\n';
        }
        at = *end == '\0' ? end : end + 1;
    }
    if (used != 0) used--;
    filtered[used] = '\0';
    budget_free(hop->budget, hop->extra_headers);
    hop->extra_headers = filtered;
    return true;
}

static bool fetch_github_release_url_allowed(const char *value)
{
    TilefinchUrl parsed;
    if (!tilefinch_url_parse(value, &parsed)
        || parsed.scheme != TILEFINCH_URL_SCHEME_HTTPS
        || parsed.port != 443 || parsed.ipv6_literal) return false;
    const char *host = value + parsed.host_offset;
    size_t host_length = parsed.host_length;
    static const char github[] = "github.com";
    static const char content[] = "githubusercontent.com";
    bool github_exact = host_length == sizeof(github) - 1u
        && strncasecmp(host, github, host_length) == 0;
    bool content_subdomain =
        host_length > sizeof(content)
        && host[host_length - sizeof(content)] == '.'
        && strncasecmp(
               host + host_length - (sizeof(content) - 1u),
               content, sizeof(content) - 1u) == 0;
    return github_exact || content_subdomain;
}

static bool fetch_hop_advance(FetchHopState *hop,
                              const FetchResult *response)
{
    if (hop == NULL || response == NULL || hop->redirects >= 5) return false;
    char location[TILEFINCH_URL_SERIALIZED_LIMIT];
    char next_logical[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!fetch_response_header_value(
            response, "location", location, sizeof(location))
        || !tilefinch_url_resolve(
               hop->logical_url, location,
               next_logical, sizeof(next_logical))) {
        return false;
    }
    /* HTTP redirect processing inherits the current logical fragment only
       when Location has no fragment delimiter. A literal trailing '#'
       explicitly replaces it with the empty fragment. */
    if (strchr(location, '#') == NULL) {
        const char *fragment = strchr(hop->logical_url, '#');
        if (fragment != NULL) {
            size_t used = strlen(next_logical);
            size_t fragment_length = strlen(fragment);
            if (fragment_length >= sizeof(next_logical) - used) return false;
            memcpy(next_logical + used, fragment, fragment_length + 1);
        }
    }
    /* Reuse the Location scratch after resolution: all subsequent transport,
       cookie, CORS, and cache decisions use the fragmentless request key. */
    char secured[TILEFINCH_URL_SERIALIZED_LIMIT];
    if (!fetch_request_prepare_security_url_with_session(
            hop->base, hop->security_session,
            next_logical, false, secured)
        || !tilefinch_url_request_key(
               secured, location, sizeof(location))) return false;
    snprintf(next_logical, sizeof(next_logical), "%s", secured);
    const char *next = location;
    const FetchRequest *base = hop->base;
    if (tilefinch_url_is_downgrade(hop->url, next)
        && !fetch_request_mixed_override(base)) return false;
    if (!fetch_request_target_allowed(base, next)) return false;
    if (fetch_origin_blocked(next)
        || fetch_request_content_blocked(base, next)) return false;
    if (base != NULL
        && base->redirect_policy == FETCH_REDIRECT_GITHUB_RELEASE
        && !fetch_github_release_url_allowed(next)) return false;
    if (base != NULL
        && base->redirect_url_validator != NULL
        && !base->redirect_url_validator(next)) return false;
    bool cross_origin = !tilefinch_url_same_origin(hop->url, next);
    /* A script request must never forward its body or author-controlled
       headers to a new origin before the caller can apply CORS.  Same-origin
       redirects remain available and are checked again at their response. */
    if (cross_origin && base != NULL
        && base->redirect_same_origin_only) return false;
    hop->client_hints_tainted = hop->client_hints_tainted || cross_origin;
    bool rewrite = response->status_code == 303
        && strcasecmp(hop->method, "HEAD") != 0;
    if ((response->status_code == 301 || response->status_code == 302)
        && strcasecmp(hop->method, "POST") == 0) rewrite = true;
    if (rewrite) {
        if (!hop_replace(hop->budget, &hop->method, "GET", 16)) return false;
        hop->body_enabled = false;
    }
    if (!fetch_hop_filter_headers(hop, cross_origin, rewrite)) return false;
    if (base != NULL
        && base->credentials == FETCH_CREDENTIALS_SAME_ORIGIN
        && (hop->credentials_tainted
            || !tilefinch_url_same_origin(base->credential_origin, next))) {
        hop->credentials_tainted = true;
    }
    bool referrer_policy_present = false;
    char response_referrer_policy[FETCH_REFERRER_POLICY_LIMIT];
    if (fetch_response_referrer_policy(
            response, &referrer_policy_present, response_referrer_policy)
        && referrer_policy_present
        && response_referrer_policy[0] != '\0') {
        snprintf(hop->referrer_policy, sizeof(hop->referrer_policy), "%s",
                 response_referrer_policy);
    }
    char referer[TILEFINCH_URL_SERIALIZED_LIMIT] = {0};
    if (hop->referrer_source != NULL) {
        if (!fetch_compute_referrer(
                hop->referrer_source, next, hop->referrer_policy,
                referer, sizeof(referer))) return false;
    }
    if (!hop_replace(hop->budget, &hop->referer, referer, 4095)) return false;
    hop->origin_tainted = hop->origin_tainted || cross_origin;
    const char *origin = NULL;
    bool omit_same_origin_origin = base != NULL && base->enforce_cors
        && fetch_method_omits_same_origin_origin(hop->method)
        && !hop->origin_tainted
        && tilefinch_url_same_origin(hop->initiator, next);
    if (base != NULL && base->origin != NULL
        && !omit_same_origin_origin) {
        origin = hop->origin_tainted ? "null" : base->origin;
    }
    if (!hop_replace(hop->budget, &hop->origin, origin, 2047)) return false;
    const char *site = hop->fetch_site_none
        ? "none"
        : (tilefinch_url_same_origin(hop->initiator, next)
              ? "same-origin" : "cross-site");
    if (!hop_replace(hop->budget, &hop->fetch_site, site, 63)
        || !hop_replace(
               hop->budget, &hop->logical_url, next_logical, 4095)
        || !hop_replace(hop->budget, &hop->url, next, 4095)) return false;
    hop->redirects++;
    return true;
}

static long fetch_hop_remaining_ms(const FetchHopState *hop)
{
    uint64_t now = tilefinch_platform_monotonic_time_ns();
    if (hop == NULL || now >= hop->deadline_ns) return 0;
    uint64_t remaining = hop->deadline_ns - now;
    uint64_t ms = (remaining + UINT64_C(999999)) / UINT64_C(1000000);
    return ms > (uint64_t) LONG_MAX ? LONG_MAX : (long) ms;
}

static bool hop_add_header(struct curl_slist **headers, const char *value)
{
    struct curl_slist *next = curl_slist_append(*headers, value);
    if (next == NULL) return false;
    *headers = next;
    return true;
}

static bool hop_add_named_header(Budget *budget,
                                 struct curl_slist **headers,
                                 const char *name, const char *value)
{
    if (value == NULL || value[0] == '\0') return true;
    size_t name_length = strlen(name), value_length = strlen(value);
    if (name_length > SIZE_MAX - value_length - 3) return false;
    size_t capacity = name_length + value_length + 3;
    char *line = budget_malloc(budget, capacity);
    if (line == NULL) return false;
    int written = snprintf(line, capacity, "%s: %s", name, value);
    bool ok = written >= 0 && (size_t) written < capacity
        && hop_add_header(headers, line);
    budget_free(budget, line);
    return ok;
}

static const char *fetch_low_client_hint_headers[] = {
    "Sec-CH-UA: \"PSP Browser\";v=\"0.3\", \"Not.A/Brand\";v=\"99\"",
    "Sec-CH-UA-Mobile: ?1",
    "Sec-CH-UA-Platform: \"PlayStation Portable\""
};

typedef struct {
    const char *token;
    const char *header;
} FetchHighClientHint;

static const FetchHighClientHint fetch_high_client_hints[] = {
    {"Sec-CH-UA-Full-Version", "Sec-CH-UA-Full-Version: \"0.3.0\""},
    {"Sec-CH-UA-Full-Version-List", "Sec-CH-UA-Full-Version-List: \"PSP Browser\";v=\"0.3.0\", \"Not.A/Brand\";v=\"99.0.0.0\""},
    {"Sec-CH-UA-Arch", "Sec-CH-UA-Arch: \"MIPS\""},
    {"Sec-CH-UA-Bitness", "Sec-CH-UA-Bitness: \"32\""},
    {"Sec-CH-UA-Model", "Sec-CH-UA-Model: \"PSP-3000\""},
    {"Sec-CH-UA-Platform-Version", "Sec-CH-UA-Platform-Version: \"6.61\""},
    {"UA", "UA: \"PSP Browser\";v=\"0.3\""},
    {"UA-Mobile", "UA-Mobile: ?1"},
    {"UA-Platform", "UA-Platform: \"PlayStation Portable\""},
    {"UA-Full-Version", "UA-Full-Version: \"0.3.0\""},
    {"UA-Full-Version-List", "UA-Full-Version-List: \"PSP Browser\";v=\"0.3.0\""},
    {"UA-Arch", "UA-Arch: \"MIPS\""},
    {"UA-Bitness", "UA-Bitness: \"32\""},
    {"UA-Model", "UA-Model: \"PSP-3000\""},
    {"UA-Platform-Version", "UA-Platform-Version: \"6.61\""}
};

static bool client_hint_token_requested(const char *serialized,
                                        const char *wanted)
{
    if (serialized == NULL || wanted == NULL) return false;
    size_t wanted_length = strlen(wanted);
    const char *at = serialized;
    while (*at != '\0') {
        while (*at == ' ' || *at == '\t') at++;
        const char *start = at;
        while (*at != '\0'
               && header_name_character((unsigned char) *at)) at++;
        size_t length = (size_t) (at - start);
        if (length == wanted_length
            && strncasecmp(start, wanted, length) == 0) return true;
        while (*at == ' ' || *at == '\t') at++;
        if (*at == ',') at++;
    }
    return false;
}

static bool client_hint_token_supported(const char *token, size_t length)
{
    for (size_t i = 0;
         i < sizeof(fetch_high_client_hints)
                 / sizeof(fetch_high_client_hints[0]); i++) {
        size_t supported_length = strlen(fetch_high_client_hints[i].token);
        if (length == supported_length
            && strncasecmp(token, fetch_high_client_hints[i].token,
                           length) == 0) return true;
    }
    return false;
}

static bool response_hint_list_contains(const char *serialized,
                                        const char *wanted,
                                        size_t wanted_length)
{
    if (serialized == NULL || wanted == NULL) return false;
    const char *at = serialized;
    while (*at != '\0') {
        while (*at == ' ' || *at == '\t') at++;
        const char *start = at;
        while (*at != '\0'
               && header_name_character((unsigned char) *at)) at++;
        if (at == start) return false;
        size_t length = (size_t) (at - start);
        while (*at == ' ' || *at == '\t') at++;
        if (length == wanted_length
            && strncasecmp(start, wanted, length) == 0) return true;
        if (*at == '\0') break;
        if (*at != ',') return false;
        at++;
    }
    return false;
}

bool fetch_accepted_critical_client_hints(
    const char *accept_ch, const char *critical_ch,
    char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) return false;
    output[0] = '\0';
    if (accept_ch == NULL || critical_ch == NULL
        || accept_ch[0] == '\0' || critical_ch[0] == '\0') return true;
    if (!client_hint_token_list_valid(accept_ch)
        || !client_hint_token_list_valid(critical_ch)) return true;
    size_t used = 0;
    const char *at = critical_ch;
    while (*at != '\0') {
        while (*at == ' ' || *at == '\t') at++;
        const char *start = at;
        while (*at != '\0'
               && header_name_character((unsigned char) *at)) at++;
        if (at == start) {
            output[0] = '\0';
            return true;
        }
        size_t length = (size_t) (at - start);
        while (*at == ' ' || *at == '\t') at++;
        bool accepted = client_hint_token_supported(start, length)
            && response_hint_list_contains(
                   accept_ch, start, length)
            && !response_hint_list_contains(output, start, length);
        if (accepted) {
            size_t separator = used == 0 ? 0 : 2;
            if (separator + length >= output_size - used) return false;
            if (separator != 0) {
                output[used++] = ',';
                output[used++] = ' ';
            }
            memcpy(output + used, start, length);
            used += length;
            output[used] = '\0';
        }
        if (*at == '\0') break;
        if (*at != ',') {
            output[0] = '\0';
            return true;
        }
        at++;
    }
    return true;
}

/* Keep the initial easy path, scheduler path, and every explicit redirect hop
   on one wire-header inventory.  origin/site/extra_headers are hop-specific;
   all other fields are immutable request policy. */
static bool append_request_headers(
    Budget *budget, struct curl_slist **headers, const FetchRequest *request,
    const char *hop_url, const char *origin, const char *fetch_site,
    const char *cookie, const char *extra_headers, bool body_enabled,
    bool suppress_high_client_hints,
    bool diagnostic_mobile_safari)
{
    if (!hop_add_header(headers, "Accept-Language: en-US,en;q=0.9")) {
        return false;
    }
    if (diagnostic_mobile_safari
        && !hop_add_header(headers, "Priority: u=0, i")) return false;
    bool trustworthy = hop_url != NULL
        && tilefinch_url_potentially_trustworthy(hop_url);
    if (!diagnostic_mobile_safari && trustworthy && request != NULL
        && request->send_low_client_hints) {
        for (size_t i = 0;
             i < sizeof(fetch_low_client_hint_headers)
                     / sizeof(fetch_low_client_hint_headers[0]); i++) {
            if (!hop_add_header(headers, fetch_low_client_hint_headers[i])) {
                return false;
            }
        }
    }
    bool high_entropy_allowed = !diagnostic_mobile_safari && request != NULL
        && trustworthy && !suppress_high_client_hints
        && request->send_client_hints
        && hop_url != NULL
        && request->client_hint_origin != NULL
        && tilefinch_url_same_origin(request->client_hint_origin, hop_url);
    if (high_entropy_allowed) {
        for (size_t i = 0;
             i < sizeof(fetch_high_client_hints)
                     / sizeof(fetch_high_client_hints[0]); i++) {
            if (client_hint_token_requested(
                    request->client_hint_tokens,
                    fetch_high_client_hints[i].token)
                && !hop_add_header(
                    headers, fetch_high_client_hints[i].header)) return false;
        }
    }
    if (!hop_add_named_header(budget, headers, "Origin", origin)
        || !hop_add_named_header(budget, headers, "Cookie", cookie)
        || !hop_add_named_header(
            budget, headers, "Accept",
            request == NULL ? NULL : request->accept)
        || !hop_add_named_header(
            budget, headers, "Sec-Fetch-Dest",
            request == NULL ? NULL : request->sec_fetch_dest)
        || !hop_add_named_header(
            budget, headers, "Sec-Fetch-Mode",
            request == NULL ? NULL : request->sec_fetch_mode)
        || !hop_add_named_header(
            budget, headers, "Sec-Fetch-Site", fetch_site)) return false;
    if (request != NULL && request->sec_fetch_user
        && !hop_add_header(headers, "Sec-Fetch-User: ?1")) return false;
    if (request != NULL && request->upgrade_insecure_requests
        && !hop_add_header(headers, "Upgrade-Insecure-Requests: 1")) {
        return false;
    }
    if (body_enabled
        && !hop_add_named_header(
            budget, headers, "Content-Type",
            request == NULL ? NULL : request->content_type)) return false;
    if (!append_extra_headers(budget, headers, extra_headers)) return false;
    return hop_add_named_header(
               budget, headers, "If-None-Match",
               request == NULL ? NULL : request->if_none_match)
        && hop_add_named_header(
            budget, headers, "If-Modified-Since",
            request == NULL ? NULL : request->if_modified_since);
}

static bool configure_easy_method(CURL *easy, const char *method,
                                  const FetchRequest *request,
                                  bool body_enabled)
{
    if (easy == NULL || method == NULL) return false;
    /* Reset every method/body selector before applying the next hop.  In
       particular, CURLOPT_POSTFIELDS (even NULL) otherwise leaves POST mode
       selected and can leak the previous hop's payload semantics. */
    if (curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, NULL) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_POSTFIELDS, NULL) != CURLE_OK
        || curl_easy_setopt(
               easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t) 0) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_POST, 0L) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_NOBODY, 0L) != CURLE_OK) {
        return false;
    }
    if (strcasecmp(method, "GET") == 0) return true;
    if (strcasecmp(method, "HEAD") == 0) {
        return curl_easy_setopt(easy, CURLOPT_NOBODY, 1L) == CURLE_OK
            && curl_easy_setopt(
                   easy, CURLOPT_CUSTOMREQUEST, "HEAD") == CURLE_OK;
    }

    const char *body = request == NULL ? NULL : request->body;
    size_t body_length = request == NULL ? 0 : request->body_length;
    bool post = strcasecmp(method, "POST") == 0;
    bool attach_body = body_enabled && request != NULL
        && (post || body != NULL || body_length != 0);
    if (attach_body
        && (curl_easy_setopt(
                easy, CURLOPT_POSTFIELDS, body == NULL ? "" : body)
                != CURLE_OK
            || curl_easy_setopt(
                   easy, CURLOPT_POSTFIELDSIZE_LARGE,
                   (curl_off_t) body_length) != CURLE_OK)) {
        return false;
    }
    if (post) {
        return body_enabled
            && curl_easy_setopt(easy, CURLOPT_POST, 1L) == CURLE_OK;
    }
    return curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method) == CURLE_OK;
}

static bool fetch_configure_redirect_hop(
    CURL *easy, const FetchRequest *request, FetchHopState *hop,
    WriteContext *context, struct curl_slist **headers)
{
    if (easy == NULL || hop == NULL || context == NULL || headers == NULL) {
        return false;
    }
    context->credentials_tainted = hop->credentials_tainted;
    curl_slist_free_all(*headers);
    *headers = NULL;
    /* A partial hop reconfiguration can retain the previous URL, Referer,
       method, or body. Treat it as one fail-closed transaction: the caller
       destroys the easy handle on any failure. */
    if (!configure_easy_method(
            easy, hop->method, request, hop->body_enabled)) return false;
    long remaining_ms = fetch_hop_remaining_ms(hop);
    if (remaining_ms <= 0) return false;
    if (curl_easy_setopt(easy, CURLOPT_URL, hop->url) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_REFERER, hop->referer) != CURLE_OK
        || curl_easy_setopt(
               easy, CURLOPT_TIMEOUT_MS, remaining_ms) != CURLE_OK
        || curl_easy_setopt(
               easy, CURLOPT_CONNECTTIMEOUT_MS,
               fetch_connect_timeout_ms(request, remaining_ms))
               != CURLE_OK) {
        return false;
    }
    if (!fetch_hop_configure_cookie(easy, hop, request, false)) return false;
    if (!append_request_headers(
            hop->budget, headers, request, hop->url, hop->origin,
            hop->fetch_site, hop->cookie_header,
            hop->extra_headers, hop->body_enabled,
            hop->client_hints_tainted,
            context->diagnostic_mobile_safari)) return false;
    return curl_easy_setopt(easy, CURLOPT_HTTPHEADER, *headers) == CURLE_OK;
}

static curl_socket_t fetch_private_network_open_socket(
    void *opaque, curlsocktype purpose, struct curl_sockaddr *address)
{
    (void) purpose;
    FetchPrivateNetworkGuard *guard = opaque;
    if (guard != NULL && guard->required
        && fetch_private_sockaddr(address == NULL ? NULL : &address->addr)) {
        guard->blocked = true;
        return CURL_SOCKET_BAD;
    }
    return address == NULL ? CURL_SOCKET_BAD
        : socket(address->family, address->socktype, address->protocol);
}

#if LIBCURL_VERSION_NUM >= 0x075000 /* 7.80.0 */
static int fetch_private_network_prerequisite(
    void *opaque, char *primary_ip, char *local_ip,
    int primary_port, int local_port)
{
    (void) local_ip; (void) primary_port; (void) local_port;
    FetchPrivateNetworkGuard *guard = opaque;
    if (guard == NULL || !guard->required) return CURL_PREREQFUNC_OK;
    struct sockaddr_storage storage;
    memset(&storage, 0, sizeof(storage));
    struct sockaddr_in *ipv4 = (struct sockaddr_in *) &storage;
    if (primary_ip != NULL
        && inet_pton(AF_INET, primary_ip, &ipv4->sin_addr) == 1) {
        ipv4->sin_family = AF_INET;
    } else {
#ifdef AF_INET6
        unsigned char ipv6[16];
        if (primary_ip != NULL
            && inet_pton(AF_INET6, primary_ip, ipv6) == 1) {
            if (fetch_private_ipv6(ipv6)) {
                guard->blocked = true;
                return CURL_PREREQFUNC_ABORT;
            }
            return CURL_PREREQFUNC_OK;
        }
#endif
        guard->blocked = true;
        return CURL_PREREQFUNC_ABORT;
    }
    if (fetch_private_sockaddr((const struct sockaddr *) &storage)) {
        guard->blocked = true;
        return CURL_PREREQFUNC_ABORT;
    }
    return CURL_PREREQFUNC_OK;
}
#endif

static bool fetch_configure_private_network(
    CURL *easy, FetchPrivateNetworkGuard *guard)
{
    if (easy == NULL || guard == NULL || !guard->required) return true;
    /* A proxy resolves the destination out of process, where this browser
       cannot inspect the peer address. Page-originated PNA requests therefore
       bypass ambient proxy variables instead of turning them into a hole. */
    if (curl_easy_setopt(easy, CURLOPT_PROXY, "") != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_OPENSOCKETFUNCTION,
                         fetch_private_network_open_socket) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_OPENSOCKETDATA, guard) != CURLE_OK)
        return false;
#if LIBCURL_VERSION_NUM >= 0x075000 /* 7.80.0 */
    return curl_easy_setopt(easy, CURLOPT_PREREQFUNCTION,
                            fetch_private_network_prerequisite) == CURLE_OK
        && curl_easy_setopt(easy, CURLOPT_PREREQDATA, guard) == CURLE_OK;
#else
    return curl_easy_setopt(easy, CURLOPT_FRESH_CONNECT, 1L) == CURLE_OK;
#endif
}

bool fetch_url(Budget *budget, const char *url, size_t maximum_bytes,
               long timeout_ms, FetchResult *result)
{
    return fetch_url_cancelable(budget, url, maximum_bytes, timeout_ms,
                                NULL, NULL, result);
}

bool fetch_url_cancelable(Budget *budget, const char *url,
                          size_t maximum_bytes, long timeout_ms,
                          FetchCancelCallback cancel, void *cancel_opaque,
                          FetchResult *result)
{
    return fetch_request_cancelable(budget, url, NULL, maximum_bytes,
                                    timeout_ms, cancel, cancel_opaque,
                                    result);
}

static bool fetch_request_internal(Budget *budget, const char *url,
                                   const FetchRequest *request,
                                   size_t maximum_bytes, long timeout_ms,
                                   FetchCancelCallback cancel,
                                   void *cancel_opaque,
                                   bool single_hop,
                                   const FetchStreamOptions *stream,
                                   FetchStreamMetrics *metrics,
                                   FetchResult *result)
{
    char normalized_logical_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    char canonical_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    const char *logical_url = normalized_logical_url;
    FetchCredentialPolicy credentials = request == NULL
        ? FETCH_CREDENTIALS_INCLUDE : request->credentials;
    const char *credential_origin = request == NULL
        ? NULL : request->credential_origin;
    if (budget == NULL || url == NULL || result == NULL || maximum_bytes == 0
        || maximum_bytes == SIZE_MAX || timeout_ms <= 0
        || !fetch_request_validate(request, NULL)
        || !tilefinch_url_normalize(
               url, normalized_logical_url,
               sizeof(normalized_logical_url))
        || !tilefinch_url_request_key(normalized_logical_url, canonical_url,
                                   sizeof(canonical_url))
        || (curl_global_users != 0 && curl_budget != budget)) {
        return false;
    }
    memset(result, 0, sizeof(*result));
    result->budget = budget;
    if (metrics != NULL) memset(metrics, 0, sizeof(*metrics));
    url = canonical_url;
    if (!page_request_target_matches(request, url)) {
        snprintf(result->error, sizeof(result->error),
                 "page request context target mismatch");
        return false;
    }
    /* The security helper snapshots its input before writing output; keep
       the upgraded logical URL in the existing buffer rather than carrying
       a third serialized URL through every synchronous request. */
    if (!fetch_request_prepare_security_url(
            request, normalized_logical_url, true, normalized_logical_url)
        || !tilefinch_url_request_key(
               normalized_logical_url, canonical_url,
               sizeof(canonical_url))) {
        snprintf(result->error, sizeof(result->error),
                 "request security upgrade failed");
        return false;
    }
    url = canonical_url;
    if (!fetch_request_target_allowed(request, url)) {
        snprintf(result->error, sizeof(result->error),
                 "blocked active mixed content");
        return false;
    }
    if (fetch_origin_blocked(url)) {
        snprintf(result->error, sizeof(result->error), "blocked origin");
        return false;
    }
    if (fetch_request_content_blocked(request, url)) {
        snprintf(result->error, sizeof(result->error),
                 "blocked by content blocker");
        return false;
    }
    if (tilefinch_fetch_consume_injected_failure(result)) return false;
    const char *method = request == NULL || request->method == NULL
                         ? "GET" : request->method;
    bool diagnostic_mobile_safari =
        getenv("TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI") != NULL;
    const char *effective_user_agent = fetch_effective_user_agent(
        request, diagnostic_mobile_safari);
    if (fetch_trace.mode == FETCH_TRACE_REPLAY) {
        return trace_replay_response(budget, method, logical_url, request,
                                     effective_user_agent,
                                     diagnostic_mobile_safari,
                                     maximum_bytes, result,
                                     stream == NULL ? NULL : &(WriteContext) {
                                         .result = result,
                                         .maximum_bytes = maximum_bytes,
                                         .cancel = cancel,
                                         .cancel_opaque = cancel_opaque,
                                         .stream = stream,
                                         .metrics = metrics
                                     }, NULL);
    }
    uint64_t checkpoint = budget_checkpoint(budget);
    char failure[sizeof(result->error)] = "network initialization failed";
    char curl_error[CURL_ERROR_SIZE] = {0};
    CURL *easy = NULL;
    struct curl_slist *headers = NULL;
    FetchHopState *hop = NULL;
    CURLcode code = CURLE_FAILED_INIT;
    long failed_transport_code = 0;
    long failed_tls_verify_result = 0;
    bool timed_out = false;
    bool tls12_retry_attempted = false;
    bool redirect_origin_tainted = false;
    bool global_acquired = false;
    WriteContext context = {
        .result = result,
        .maximum_bytes = maximum_bytes,
        .cancel = cancel,
        .cancel_opaque = cancel_opaque,
        .single_hop = single_hop,
        .stream = stream,
        .metrics = metrics,
        .credentials = credentials,
        .credential_origin = credential_origin,
        .diagnostic_mobile_safari = diagnostic_mobile_safari
    };
    if (!curl_global_acquire(budget)) goto failed;
    global_acquired = true;
    easy = curl_easy_init();
    if (easy == NULL) goto failed;
    hop = fetch_hop_create(
        budget, url, logical_url, request, timeout_ms);
    if (hop == NULL) {
        snprintf(failure, sizeof(failure),
                 "redirect policy initialization exceeded its budget");
        goto failed;
    }
    context.easy = easy;
    context.hop = hop;
    context.credentials_tainted = hop->credentials_tainted;
    hop->private_network.required =
        fetch_request_private_network_protected(request, url);
#if defined(TILEFINCH_CURL_IMPERSONATE)
    code = curl_easy_impersonate(easy, TILEFINCH_CURL_IMPERSONATE_TARGET, 1);
    if (code != CURLE_OK) {
        snprintf(failure, sizeof(failure),
                 "could not initialize browser transport profile");
        goto failed;
    }
#endif

    if (!curl_easy_attach_shared_state(easy)
        || !fetch_configure_tls(easy)
        || !fetch_configure_http_version(easy)
        || !fetch_configure_private_network(
               easy, &hop->private_network)
        || curl_easy_setopt(easy, CURLOPT_URL, url) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK
        || curl_easy_setopt(
               easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L) != CURLE_OK
        || curl_easy_setopt(
               easy, TILEFINCH_CURL_PROTOCOLS_OPT,
               TILEFINCH_CURL_HTTP_PROTOCOLS) != CURLE_OK
        || curl_easy_setopt(
               easy, TILEFINCH_CURL_REDIR_PROTOCOLS_OPT,
               TILEFINCH_CURL_HTTP_PROTOCOLS) != CURLE_OK
        || curl_easy_setopt(
               easy, CURLOPT_ACCEPT_ENCODING,
               request != NULL && request->identity_encoding
                   ? "identity"
                   : fetch_accept_encoding(
                         diagnostic_mobile_safari)) != CURLE_OK
        || curl_easy_setopt(
               easy, CURLOPT_USERAGENT, effective_user_agent) != CURLE_OK
        || curl_easy_setopt(
               easy, CURLOPT_CONNECTTIMEOUT_MS,
               fetch_connect_timeout_ms(request, timeout_ms)) != CURLE_OK
        || curl_easy_setopt(
               easy, CURLOPT_FRESH_CONNECT,
               request != NULL && request->force_fresh_connection
                   ? 1L : 0L) != CURLE_OK
        || curl_easy_setopt(
               easy, CURLOPT_TIMEOUT_MS, timeout_ms) != CURLE_OK
        || !fetch_configure_stall_watchdog(easy)
        || curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) != CURLE_OK
        || curl_easy_setopt(
               easy, CURLOPT_FAILONERROR,
               request != NULL && request->allow_http_errors
                   ? 0L : 1L) != CURLE_OK
        || curl_easy_setopt(
               easy, CURLOPT_ERRORBUFFER, curl_error) != CURLE_OK
        || curl_easy_setopt(
               easy, CURLOPT_WRITEFUNCTION, receive_data) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_WRITEDATA, &context) != CURLE_OK
        || curl_easy_setopt(
               easy, CURLOPT_HEADERFUNCTION, receive_header) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_HEADERDATA, &context) != CURLE_OK
        || curl_easy_setopt(
               easy, CURLOPT_XFERINFOFUNCTION,
               transfer_progress) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &context) != CURLE_OK
        || curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L) != CURLE_OK) {
        snprintf(failure, sizeof(failure),
                 "could not configure network request");
        goto failed;
    }
    if (!fetch_hop_configure_cookie(easy, hop, request, true)) {
        snprintf(failure, sizeof(failure),
                 "request cookies exceeded URL or memory policy");
        goto failed;
    }
    if (!configure_easy_method(easy, method, request, true)) {
        snprintf(failure, sizeof(failure),
                 "could not configure request method or body");
        goto failed;
    }
    if (request != NULL) {
        if (request->referer != NULL && request->referer[0] != '\0'
            && curl_easy_setopt(
                   easy, CURLOPT_REFERER, request->referer) != CURLE_OK) {
            snprintf(failure, sizeof(failure),
                     "could not configure request referrer");
            goto failed;
        }
    }
    if (!append_request_headers(
            budget, &headers, request, url,
            request == NULL ? NULL : request->origin,
            request == NULL ? NULL : request->sec_fetch_site,
            hop->cookie_header,
            request == NULL ? NULL : request->extra_headers, true, false,
            diagnostic_mobile_safari)
        || curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers) != CURLE_OK) {
        snprintf(failure, sizeof(failure),
                 "request headers exceeded policy or memory budget");
        goto failed;
    }
    /* Do not consume the armed ordinal until every ordinary request resource
       is ready. Failures above remain preparatory and leave capture armed. */
    if (!trace_activate_for_request(request, url)) {
        snprintf(failure, sizeof(failure),
                 "delayed HTTP trace activation failed");
        goto failed;
    }
    if (stream != NULL && fetch_trace.mode == FETCH_TRACE_CAPTURE) {
        size_t sequence = fetch_trace.sequence;
        if (sequence >= FETCH_TRACE_RECORD_LIMIT) {
            trace_capture_invalidate("record limit exceeded", sequence);
            snprintf(failure, sizeof(failure),
                     "HTTP trace record limit exceeded");
            goto failed;
        }
        context.capture_body_path = budget_malloc(
            budget, FETCH_TRACE_PATH_CAPACITY);
        if (context.capture_body_path == NULL) {
            context.capture_failed = true;
            snprintf(failure, sizeof(failure),
                     "HTTP stream capture path exceeded shared memory budget");
            goto failed;
        }
        if (!trace_path(context.capture_body_path,
                        FETCH_TRACE_PATH_CAPACITY, sequence, "body")) {
            context.capture_failed = true;
            snprintf(failure, sizeof(failure),
                     "HTTP stream capture path is too long");
            goto failed;
        }
        context.capture_body = fopen(context.capture_body_path, "wb");
        if (context.capture_body == NULL) {
            context.capture_failed = true;
            snprintf(failure, sizeof(failure),
                     "HTTP stream capture body could not be opened");
            goto failed;
        }
        context.capture_sequence = sequence;
        context.has_capture_sequence = true;
        fetch_trace.sequence++;
    }
    for (;;) {
        curl_error[0] = '\0';
        code = curl_easy_perform(easy);
        if (!hop->private_network.blocked
            && code == CURLE_SSL_CONNECT_ERROR
            && !context.headers_delivered
            && result->received_body_bytes == 0
            && fetch_try_tls12_compatibility(
                   easy, code, &tls12_retry_attempted,
                   fetch_hop_remaining_ms(hop), request)) {
            result->tls12_compatibility_retry = true;
            continue;
        }
        bool redirect_callback_abort =
            tilefinch_fetch_redirect_callback_abort_expected(
                code, context.redirect_response,
                context.redirect_callback_aborted,
                redirect_callback_failed(&context));
        if (code != CURLE_OK && !redirect_callback_abort) {
            timed_out = code == CURLE_OPERATION_TIMEDOUT;
            const char *detail = hop->private_network.blocked
                                 ? "blocked private network target"
                                 : (context.cancelled
                                 ? "request cancelled"
                                 : (context.cookie_header_rejected
                                 && result->error[0] != '\0'
                                 ? result->error
                                 : (context.client_hint_header_rejected
                                 && result->error[0] != '\0'
                                 ? result->error
                                 : (context.cors_rejected
                                 && result->error[0] != '\0'
                                 ? result->error
                                 : (context.limit_exceeded
                                 ? "response exceeded --max-download-kb"
                                 : (context.capture_failed
                                    ? "HTTP stream capture write failed"
                                 : (context.consumer_failed
                                    ? "stream consumer rejected response"
                                 : (context.allocation_failed
                                    ? "response exceeded shared memory budget"
                                    : (curl_error[0] != '\0'
                                       ? curl_error
                                       : curl_easy_strerror(code))))))))));
            (void) snprintf(failure, sizeof(failure), "%s", detail);
            goto failed;
        }
        /* Every hop of an explicit redirect chain is its own libcurl transfer
           with its own connect/handshake timers, so attribute each one before
           the next hop overwrites them.  FetchResult keeps the final hop's
           numbers, matching the other transport-info fields. */
        fetch_tls_counters_record(easy, NULL);
        if (request != NULL && request->cookie_session != NULL) {
            (void) browser_session_hsts_observe_metadata(
                (BrowserSession *) request->cookie_session,
                hop->url, &result->security);
        }
        if (!context.redirect_response) break;
        if (hop->redirects >= 5) {
            snprintf(failure, sizeof(failure),
                     "response exceeded the five-hop redirect limit");
            goto failed;
        }
        fetch_hop_stage_response_cookies(
            hop, result, context.response_cookie_start);
        if (!fetch_hop_advance(hop, result)) {
            snprintf(failure, sizeof(failure),
                     "response redirect target is invalid");
            goto failed;
        }
        if (!fetch_configure_redirect_hop(
                easy, request, hop, &context, &headers)) {
            timed_out = fetch_hop_remaining_ms(hop) <= 0;
            snprintf(failure, sizeof(failure), "%s",
                     timed_out
                        ? "redirect chain exceeded the request deadline"
                        : "redirect request headers exceeded policy or memory budget");
            goto failed;
        }
        context.redirect_response = false;
        context.redirect_callback_aborted = false;
        context.interim_response = false;
        context.headers_delivered = false;
    }

    char *effective = NULL;
    char *content_type = NULL;
    (void) curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &result->status_code);
    (void) curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &effective);
    (void) curl_easy_getinfo(easy, CURLINFO_CONTENT_TYPE, &content_type);
    result->transport_code = (long) code;
    fetch_result_set_transport_info(easy, result);
    write_context_set_effective_url(&context, effective);
    if (content_type != NULL) {
        (void) snprintf(result->content_type, sizeof(result->content_type),
                        "%s", content_type);
    }
    if (stream == NULL && result->data == NULL) {
        result->data = budget_malloc(budget, 1);
        if (result->data == NULL) {
            (void) snprintf(failure, sizeof(failure), "empty response allocation failed");
            goto failed;
        }
        result->data[0] = '\0';
        result->capacity = 1;
    }
    curl_slist_free_all(headers);
    headers = NULL;
    fetch_hop_destroy(hop);
    hop = NULL;
    curl_easy_cleanup(easy);
    easy = NULL;
    curl_global_release();
    global_acquired = false;
    if (context.capture_body != NULL) {
        if (fclose(context.capture_body) != 0) context.capture_failed = true;
        context.capture_body = NULL;
        if (context.capture_failed) {
            snprintf(failure, sizeof(failure),
                     "HTTP stream capture finalization failed");
            goto failed;
        }
    }
    if (stream == NULL && result->capacity > result->length + 1) {
        char *trimmed = budget_realloc(budget, result->data,
                                       result->length + 1);
        if (trimmed != NULL) {
            result->data = trimmed;
            result->capacity = result->length + 1;
        }
    }
    if (fetch_trace.mode == FETCH_TRACE_CAPTURE && stream == NULL) {
        size_t sequence = fetch_trace.sequence;
        if (!trace_capture_response(
                method, url, logical_url, request, effective_user_agent,
                diagnostic_mobile_safari, result, true)) {
            trace_capture_invalidate("synchronous record write failed",
                                     sequence);
        }
    }
    if (fetch_trace.mode == FETCH_TRACE_CAPTURE && stream != NULL) {
        bool captured = context.has_capture_sequence
            && !context.capture_failed
            && trace_capture_response_at(
                context.capture_sequence, method, url, logical_url, request,
                effective_user_agent, diagnostic_mobile_safari,
                result, true, 0, false, true);
        if (!captured) {
            trace_capture_invalidate("streaming record write failed",
                                     context.capture_sequence);
        }
    }
    budget_free(budget, context.capture_body_path);
    context.capture_body_path = NULL;
    fetch_result_log_transport(result);
    return true;

failed:
    failed_transport_code = (long) code;
    failed_tls_verify_result = 0;
    if (easy != NULL) {
        fetch_result_set_transport_info(easy, result);
        failed_tls_verify_result = result->tls_verify_result;
    }
    redirect_origin_tainted = result->redirect_origin_tainted
        || (hop != NULL && hop->origin_tainted);
    if (context.capture_body != NULL) {
        if (fclose(context.capture_body) != 0) {
            context.capture_failed = true;
        }
        context.capture_body = NULL;
    }
    if (context.capture_failed && context.capture_body_path != NULL
        && context.capture_body_path[0] != '\0') {
        (void) remove(context.capture_body_path);
    }
    curl_slist_free_all(headers);
    fetch_hop_destroy(hop);
    if (easy != NULL) curl_easy_cleanup(easy);
    if (global_acquired) curl_global_release();
    fetch_result_discard_all_cookie_storage(result);
    /* A streaming consumer may have created parser/runtime state from body
       callbacks after this request's checkpoint. Those allocations are not
       transport-owned, so the consumer must unwind them transactionally. */
    if (stream == NULL) {
        budget_rollback(budget, checkpoint);
        memset(result, 0, sizeof(*result));
        result->budget = budget;
    }
    result->timed_out = timed_out;
    result->transport_code = failed_transport_code;
    result->tls_verify_result = failed_tls_verify_result;
    result->redirect_origin_tainted = redirect_origin_tainted;
    (void) snprintf(result->error, sizeof(result->error), "%s", failure);
    if (fetch_trace.mode == FETCH_TRACE_CAPTURE && stream == NULL) {
        size_t sequence = fetch_trace.sequence;
        if (!trace_capture_response(
                method, url, logical_url, request, effective_user_agent,
                diagnostic_mobile_safari, result, false)) {
            trace_capture_invalidate("synchronous failure record write failed",
                                     sequence);
        }
    }
    if (fetch_trace.mode == FETCH_TRACE_CAPTURE && stream != NULL) {
        bool captured = false;
        if (!context.capture_failed) {
            captured = context.has_capture_sequence
                ? trace_capture_response_at(
                      context.capture_sequence, method, url, logical_url,
                      request,
                      effective_user_agent, diagnostic_mobile_safari,
                      result, false, 0, context.cancelled, true)
                : trace_capture_response(
                      method, url, logical_url, request, effective_user_agent,
                      diagnostic_mobile_safari, result, false);
        }
        if (!captured) {
            size_t sequence = context.has_capture_sequence
                ? context.capture_sequence : fetch_trace.sequence;
            trace_capture_invalidate("streaming failure record write failed",
                                     sequence);
        }
    }
    budget_free(budget, context.capture_body_path);
    context.capture_body_path = NULL;
    fetch_result_log_transport(result);
    return false;
}

bool fetch_request_cancelable(Budget *budget, const char *url,
                              const FetchRequest *request,
                              size_t maximum_bytes, long timeout_ms,
                              FetchCancelCallback cancel, void *cancel_opaque,
                              FetchResult *result)
{
    return fetch_request_internal(budget, url, request, maximum_bytes,
                                  timeout_ms, cancel, cancel_opaque,
                                  false, NULL, NULL, result);
}

bool fetch_request_single_hop(
    Budget *budget, const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, FetchResult *result)
{
    return fetch_request_internal(
        budget, url, request, maximum_bytes, timeout_ms,
        NULL, NULL, true, NULL, NULL, result);
}

bool fetch_request_stream_cancelable(
    Budget *budget, const char *url, const FetchRequest *request,
    size_t maximum_bytes, long timeout_ms, FetchCancelCallback cancel,
    void *cancel_opaque, const FetchStreamOptions *stream,
    FetchStreamMetrics *metrics, FetchResult *result)
{
    if (stream == NULL || stream->on_body == NULL) return false;
    return fetch_request_internal(budget, url, request, maximum_bytes,
                                  timeout_ms, cancel, cancel_opaque,
                                  false, stream, metrics, result);
}

#define FETCH_SCHEDULER_LIMIT 16u

typedef struct {
    uint64_t id;
    CURL *easy;
    struct curl_slist *headers;
    FetchHopState *hop;
    FetchRequest request;
    /* Non-owning engine/session-lifetime target for HSTS observations. The
       request-private cookie overlay remains the only mutable cookie state. */
    BrowserSession *security_session;
    FetchResult result;
    WriteContext writer;
    char *url;
    char *logical_url;
    char *method;
    char *body;
    char *content_type;
    char *extra_headers;
    char *cookie;
    /* Capture owns the initial Cookie header actually configured on the
       transport.  The live request's session/context pointers may be
       stack-backed and are deliberately dropped after hop construction. */
    char *trace_cookie;
    char *referer;
    char *origin;
    char *accept;
    char *fetch_dest;
    char *fetch_mode;
    char *fetch_site;
    char *client_hint_tokens;
    char *client_hint_origin;
    char *if_none_match;
    char *if_modified_since;
    char *user_agent;
    char *credential_origin;
    char *initiator_url;
    char *page_top_level_url;
    TilefinchRequestContext page_context;
    char *referrer_source;
    char *referrer_policy;
    char error[CURL_ERROR_SIZE];
    size_t reserved_bytes;
    bool active;
    bool complete;
    bool success;
    unsigned replay_pumps_remaining;
    size_t trace_sequence;
    bool has_trace_sequence;
    size_t enqueue_pump_generation;
    FetchStreamOptions stream;
    FetchStreamMetrics stream_metrics;
    FILE *replay_file;
    unsigned char *replay_buffer;
    size_t replay_buffer_capacity;
    size_t replay_length;
    size_t replay_offset;
    size_t replay_chunk_remaining;
    uint64_t replay_random;
    bool streaming;
    bool replay_source_success;
    bool replay_terminal_matched;
    /* Strict sequential replay keeps a captured external cancellation pending
       until the current caller cancels. Response-keyed visual replay instead
       reproduces the retained terminal rejection at its recorded delay. */
    bool replay_retained_external_cancel;
    bool replay_headers_delivered;
    bool capture_finalized;
    bool transport_complete;
    uint64_t background_request_id;
    bool background_backend;
    bool background_allowed;
    bool background_headers_delivered;
    bool domain_slot_reserved;
    bool tls12_retry_attempted;
    CURLcode transport_result;
} ScheduledFetch;

struct FetchSchedulerDomain {
    Budget *budget;
    size_t references;
    size_t maximum_active_slots;
    size_t active_slots;
    size_t peak_active_slots;
    size_t rejected_enqueues;
    size_t active_views;
    size_t peak_active_views;
};

struct FetchScheduler {
    Budget *budget;
    CURLM *multi;
    ScheduledFetch *items[FETCH_SCHEDULER_LIMIT];
    FetchSchedulerDomain *domain;
    size_t capacity;
    size_t maximum_reserved_bytes;
    size_t reserved_bytes;
    size_t pending;
    uint64_t next_id;
    size_t pump_generation;
    size_t pump_cursor;
    bool global_acquired;
    bool background_transport_enabled;
    unsigned char *background_chunk;
    size_t background_chunk_capacity;
    FetchTlsHandshakeCounters tls_counters;
    char last_error[96];
};
