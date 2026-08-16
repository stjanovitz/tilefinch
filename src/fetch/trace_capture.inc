static bool trace_replay_record_claimed(size_t sequence)
{
    return sequence >= fetch_trace.replay_record_count
        || (fetch_trace.replay_claimed[sequence / 64u]
            & (UINT64_C(1) << (sequence % 64u))) != 0;
}

static void trace_replay_claim_record(size_t sequence)
{
    if (sequence >= fetch_trace.replay_record_count) return;
    fetch_trace.replay_claimed[sequence / 64u]
        |= UINT64_C(1) << (sequence % 64u);
}

static uint64_t trace_wall_time_ms(void)
{
    return tilefinch_platform_wall_time_ns() / UINT64_C(1000000);
}

static bool trace_path(char *output, size_t capacity, size_t sequence,
                       const char *suffix)
{
    int written = snprintf(output, capacity, "%s/%04zu.%s",
                           fetch_trace.directory, sequence, suffix);
    return written >= 0 && (size_t) written < capacity;
}

static bool trace_safe_line(const char *value)
{
    if (value == NULL) return true;
    return strchr(value, '\n') == NULL && strchr(value, '\r') == NULL;
}

static bool trace_write_line(FILE *file, const char *name, const char *value)
{
    return trace_safe_line(value)
        && fprintf(file, "%s=%s\n", name, value == NULL ? "" : value) >= 0;
}

/* The capture boundary needs the cookie jar's policy shape so replay can
   rebuild byte-identical Cookie headers without retaining credentials.  An
   expired entry is already absent from the logical jar. Empty values remain
   meaningful and are represented by value-length=0. */
static bool trace_write_cookie_seed(FILE *file,
                                    const BrowserSession *session)
{
    if (file == NULL) return false;
    int64_t now = (int64_t) time(NULL);
    size_t count = 0;
    if (session != NULL) {
        for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
            const BrowserCookieEntry *entry = &session->cookies[i];
            if (entry->value == NULL
                || (entry->expires_at != 0 && entry->expires_at <= now)) {
                continue;
            }
            if (entry->creation_sequence == 0
                || entry->creation_sequence == SIZE_MAX) return false;
            /* Replay seeds deliberately remain fixed-size and redacted.
               Browsing accepts exceptional paths through the separately
               bounded long-path pool, but a capture containing one must fail
               explicitly instead of publishing metadata its parser would
               truncate or reject later. */
            if (entry->path_length >= BROWSER_ORIGIN_LIMIT) return false;
            count++;
        }
    }
    if (fprintf(file, "cookie-seed-version=1\n"
                      "cookie-seed-count=%zu\n", count) < 0) return false;
    size_t index = 0;
    if (session == NULL) return true;
    for (size_t i = 0; i < BROWSER_COOKIE_ENTRIES; i++) {
        const BrowserCookieEntry *entry = &session->cookies[i];
        if (entry->value == NULL
            || (entry->expires_at != 0 && entry->expires_at <= now)) {
            continue;
        }
        char name[64];
#define TRACE_COOKIE_TEXT(field, value) do {                                \
    int written = snprintf(name, sizeof(name),                              \
                           "cookie-seed-%zu-%s", index, (field));          \
    if (written < 0 || (size_t) written >= sizeof(name)                     \
        || !trace_write_line(file, name, (value))) return false;            \
} while (0)
#define TRACE_COOKIE_NUMBER(field, format, value) do {                      \
    if (fprintf(file, "cookie-seed-%zu-%s=" format "\n",                 \
                index, (field), (value)) < 0) return false;                 \
} while (0)
        TRACE_COOKIE_TEXT("domain", entry->domain);
        TRACE_COOKIE_TEXT("path", browser_cookie_entry_path(entry));
        TRACE_COOKIE_TEXT("name", entry->name);
        TRACE_COOKIE_NUMBER("value-length", "%zu", entry->value_length);
        TRACE_COOKIE_NUMBER("host-only", "%d", entry->host_only ? 1 : 0);
        TRACE_COOKIE_NUMBER("secure", "%d", entry->secure ? 1 : 0);
        TRACE_COOKIE_NUMBER("http-only", "%d", entry->http_only ? 1 : 0);
        TRACE_COOKIE_NUMBER("same-site", "%u", (unsigned) entry->same_site);
        TRACE_COOKIE_NUMBER("partitioned", "%d",
                            entry->partitioned ? 1 : 0);
        TRACE_COOKIE_TEXT("partition-key", entry->partition_key);
        TRACE_COOKIE_NUMBER("creation-sequence", "%zu",
                            entry->creation_sequence);
#undef TRACE_COOKIE_NUMBER
#undef TRACE_COOKIE_TEXT
        index++;
    }
    return index == count;
}

/* Preserve cookie names, attributes, and byte lengths so deterministic replay
   still exercises cookie policy without persisting live session secrets. */
static bool trace_cookie_value(const char *cookie, char *output,
                               size_t capacity, bool retain_raw)
{
    if (cookie == NULL || output == NULL || capacity == 0) return false;
    size_t length = strlen(cookie);
    if (length >= capacity) return false;
    memcpy(output, cookie, length + 1);
    if (retain_raw) return true;
    char *equals = strchr(output, '=');
    if (equals == NULL) return true;
    char *end = strchr(equals + 1, ';');
    if (end == NULL) end = output + length;
    for (char *at = equals + 1; at < end; at++) {
        if (*at != '"') *at = 'x';
    }
    return true;
}

static size_t response_header_count(const FetchResult *result)
{
    size_t count = 0;
    for (size_t i = 0; i < result->response_headers_length; i++) {
        if (result->response_headers[i] == '\n') count++;
    }
    return count;
}

static uint64_t trace_body_hash_with_offset(const char *body, size_t length,
                                            uint64_t offset)
{
    uint64_t hash = offset;
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char) body[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t trace_body_hash(const char *body, size_t length)
{
    return trace_body_hash_with_offset(
        body, length, UINT64_C(14695981039346656037));
}

static uint64_t trace_body_hash_v1(const char *body, size_t length)
{
    return trace_body_hash_with_offset(
        body, length, UINT64_C(1469598103934665603));
}

static bool trace_open_body_hash(FILE *file, size_t length, uint64_t *hash)
{
    if (file == NULL || hash == NULL) return false;
    unsigned char buffer[4096];
    size_t remaining = length;
    uint64_t value = trace_body_hash("", 0);
    bool ok = true;
    while (remaining != 0) {
        size_t wanted = remaining < sizeof(buffer) ? remaining
                                                   : sizeof(buffer);
        if (fread(buffer, 1, wanted, file) != wanted) {
            ok = false;
            break;
        }
        value = trace_body_hash_with_offset(
            (const char *) buffer, wanted, value);
        remaining -= wanted;
    }
    if (ok) ok = fgetc(file) == EOF && !ferror(file);
    clearerr(file);
    if (fseek(file, 0, SEEK_SET) != 0) ok = false;
    if (ok) *hash = value;
    return ok;
}

static bool trace_file_body_hash(const char *path, size_t length,
                                 uint64_t *hash)
{
    if (path == NULL || hash == NULL) return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    bool ok = trace_open_body_hash(file, length, hash);
    if (fclose(file) != 0) ok = false;
    return ok;
}

/* Preserve application-header identity without retaining bearer tokens or
   other header values. Header names and exact serialized value byte lengths
   are sufficient to reject a differently shaped replay request, while the
   trace remains deliberately unable to authenticate a secret value. */
static bool trace_extra_header_shape(const char *serialized, char *output,
                                     size_t capacity)
{
    if (output == NULL || capacity == 0) return false;
    output[0] = '\0';
    if (serialized == NULL || serialized[0] == '\0') return true;
    size_t used = 0;
    for (const char *at = serialized; *at != '\0';) {
        const char *end = strchr(at, '\n');
        if (end == NULL) end = at + strlen(at);
        const char *colon = memchr(at, ':', (size_t) (end - at));
        if (colon == NULL || colon == at) return false;
        if (used != 0) {
            if (used + 1 >= capacity) return false;
            output[used++] = ',';
        }
        for (const char *name = at; name < colon; name++) {
            if (used + 1 >= capacity) return false;
            output[used++] = (char) tolower((unsigned char) *name);
        }
        if (used + 1 >= capacity) return false;
        output[used++] = ':';
        size_t value_length = (size_t) (end - colon - 1);
        int written = snprintf(output + used, capacity - used, "%zu",
                               value_length);
        if (written < 0 || (size_t) written >= capacity - used) return false;
        used += (size_t) written;
        at = *end == '\0' ? end : end + 1;
    }
    return true;
}

typedef struct {
    char meta_path[4096];
    char body_path[4096];
    char logical_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    char request_key[TILEFINCH_URL_SERIALIZED_LIMIT];
    char header_value[FETCH_EXTRA_HEADERS_MAX + 1];
} TraceCaptureScratch;

static bool trace_capture_response_at(size_t sequence, const char *method,
                                      const char *url,
                                      const char *logical_url,
                                      const FetchRequest *request,
                                      const char *effective_user_agent,
                                      bool diagnostic_mobile_safari,
                                      const FetchResult *result, bool success,
                                      size_t delay_pumps,
                                      bool external_cancel,
                                      bool body_previously_written)
{
    Budget *budget = result->budget;
    TraceCaptureScratch *scratch = budget_malloc(budget, sizeof(*scratch));
    if (scratch == NULL) return false;
    if (!trace_path(scratch->meta_path, sizeof(scratch->meta_path), sequence,
                    "meta")
        || !trace_path(scratch->body_path, sizeof(scratch->body_path),
                       sequence, "body")
        || !tilefinch_url_normalize(logical_url, scratch->logical_url,
                                 sizeof(scratch->logical_url))
        || !tilefinch_url_request_key(scratch->logical_url,
                                   scratch->request_key,
                                   sizeof(scratch->request_key))
        || strcmp(scratch->request_key, url) != 0) {
        budget_free(budget, scratch);
        return false;
    }
    uint64_t response_body_hash = 0;
    if (body_previously_written) {
        if (!trace_file_body_hash(scratch->body_path, result->length,
                                  &response_body_hash)) {
            budget_free(budget, scratch);
            return false;
        }
    } else {
        if (result->length != 0 && result->data == NULL) {
            budget_free(budget, scratch);
            return false;
        }
        response_body_hash = trace_body_hash(
            result->data == NULL ? "" : result->data, result->length);
    }
    if (!trace_extra_header_shape(
            request == NULL ? NULL : request->extra_headers,
            scratch->header_value, sizeof(scratch->header_value))) {
        budget_free(budget, scratch);
        return false;
    }
    FetchResponseSecurityMetadata captured_security = result->security;
    if (captured_security.version
            != FETCH_RESPONSE_SECURITY_METADATA_VERSION
        && !fetch_response_security_metadata_from_snapshot(
            &captured_security, result->response_headers,
            result->response_headers_length,
            result->response_security_headers_truncated)) {
        budget_free(budget, scratch);
        return false;
    }
    FILE *meta = fopen(scratch->meta_path, "wb");
    FILE *body = body_previously_written
        ? NULL : fopen(scratch->body_path, "wb");
    if (meta == NULL || (!body_previously_written && body == NULL)) {
        if (meta != NULL) fclose(meta);
        if (body != NULL) fclose(body);
        budget_free(budget, scratch);
        return false;
    }
    bool ok = fprintf(meta, "psp-http-trace=11\n") >= 0
        && fprintf(meta, "cookie-values=%s\n",
                   fetch_trace.capture_raw_cookie_values
                       ? "raw" : "redacted") >= 0
        && trace_write_line(meta, "method", method)
        && trace_write_line(meta, "url", url)
        && trace_write_line(meta, "logical-request-url", scratch->logical_url)
        && fprintf(meta, "success=%d\n", success ? 1 : 0) >= 0
        && fprintf(meta, "async-delay-pumps=%zu\n", delay_pumps) >= 0
        && fprintf(meta, "external-cancel=%d\n",
                   external_cancel ? 1 : 0) >= 0
        && fprintf(meta, "transport-timeout=%d\n",
                   result->timed_out ? 1 : 0) >= 0
        && fprintf(meta, "redirect-origin-tainted=%d\n",
                   result->redirect_origin_tainted ? 1 : 0) >= 0
        && trace_write_line(meta, "error", result->error)
        && fprintf(meta, "request-body-length=%zu\n",
                   request == NULL ? 0 : request->body_length) >= 0
        && fprintf(meta, "request-body-hash=%016llx\n",
                   (unsigned long long) trace_body_hash(
                       request == NULL || request->body == NULL
                         ? "" : request->body,
                       request == NULL ? 0 : request->body_length)) >= 0
        && trace_write_line(
            meta, "request-content-type",
            request == NULL ? NULL : request->content_type)
        && fprintf(meta, "request-cookie-bytes=%zu\n",
                   request == NULL || request->cookie == NULL
                     ? 0 : strlen(request->cookie)) >= 0
        && fprintf(meta, "request-has-cf-clearance=%d\n",
                   request != NULL && request->cookie != NULL
                   && strstr(request->cookie, "cf_clearance=") != NULL) >= 0
        && fprintf(meta, "request-extra-header-bytes=%zu\n",
                   request == NULL || request->extra_headers == NULL
                     ? 0 : strlen(request->extra_headers)) >= 0
        && trace_write_line(meta, "request-extra-header-shape",
                            scratch->header_value)
        && fprintf(meta, "request-allow-http-errors=%d\n",
                   request != NULL && request->allow_http_errors) >= 0
        && fprintf(meta, "request-enforce-cors=%d\n",
                   request != NULL && request->enforce_cors) >= 0
        && fprintf(meta, "request-redirect-same-origin-only=%d\n",
                   request != NULL
                   && request->redirect_same_origin_only) >= 0
        && fprintf(meta, "request-cors-cached-response-validated=%d\n",
                   request != NULL
                   && request->cors_cached_response_validated) >= 0
        && trace_write_line(
            meta, "request-if-none-match",
            request == NULL ? NULL : request->if_none_match)
        && trace_write_line(
            meta, "request-if-modified-since",
            request == NULL ? NULL : request->if_modified_since)
        && trace_write_line(meta, "request-referer",
                            request == NULL ? NULL : request->referer)
        && trace_write_line(meta, "request-origin",
                            request == NULL ? NULL : request->origin)
        && trace_write_line(meta, "request-accept",
                            request == NULL ? NULL : request->accept)
        && trace_write_line(
            meta, "request-sec-fetch-dest",
            request == NULL ? NULL : request->sec_fetch_dest)
        && trace_write_line(
            meta, "request-sec-fetch-mode",
            request == NULL ? NULL : request->sec_fetch_mode)
        && trace_write_line(
            meta, "request-sec-fetch-site",
            request == NULL ? NULL : request->sec_fetch_site)
        && fprintf(meta, "request-send-client-hints=%d\n",
                   request != NULL && request->send_client_hints) >= 0
        && trace_write_line(
            meta, "request-client-hint-tokens",
            request == NULL ? NULL : request->client_hint_tokens)
        && trace_write_line(
            meta, "request-client-hint-origin",
            request == NULL ? NULL : request->client_hint_origin)
        && fprintf(meta, "request-send-low-client-hints=%d\n",
                   request != NULL && request->send_low_client_hints) >= 0
        && fprintf(meta, "request-sec-fetch-user=%d\n",
                   request != NULL && request->sec_fetch_user) >= 0
        && fprintf(meta, "request-upgrade-insecure=%d\n",
                   request != NULL
                   && request->upgrade_insecure_requests) >= 0
        && trace_write_line(meta, "request-user-agent",
                            effective_user_agent)
        && fprintf(meta, "request-diagnostic-mobile-safari=%d\n",
                   diagnostic_mobile_safari ? 1 : 0) >= 0
        && fprintf(meta, "request-credentials=%d\n",
                   request == NULL ? (int) FETCH_CREDENTIALS_INCLUDE
                                   : (int) request->credentials) >= 0
        && trace_write_line(
            meta, "request-credential-origin",
            request == NULL ? NULL : request->credential_origin)
        && trace_write_line(
            meta, "request-initiator-url",
            request == NULL ? NULL : request->initiator_url)
        && trace_write_line(
            meta, "request-referrer-source",
            request == NULL ? NULL : request->referrer_source)
        && trace_write_line(
            meta, "request-referrer-policy",
            request == NULL ? NULL : request->referrer_policy)
        && fprintf(meta, "status=%ld\n", result->status_code) >= 0
        && fprintf(meta, "length=%zu\n", result->length) >= 0
        && fprintf(meta, "response-body-hash=%016llx\n",
                   (unsigned long long) response_body_hash) >= 0
        && trace_write_line(meta, "effective-url", result->effective_url)
        && trace_write_line(meta, "content-type", result->content_type)
        && trace_write_line(meta, "etag", result->etag)
        && trace_write_line(meta, "last-modified", result->last_modified)
        && trace_write_line(meta, "cf-mitigated", result->cf_mitigated)
        && trace_write_line(meta, "accept-ch", result->accept_ch)
        && trace_write_line(meta, "critical-ch", result->critical_ch)
        && trace_write_line(meta, "server", result->server)
        && trace_write_line(meta, "cf-ray", result->cf_ray)
        && fprintf(meta, "response-referrer-policy-metadata-valid=%d\n",
                   result->response_referrer_policy_metadata_valid ? 1 : 0)
                   >= 0
        && fprintf(meta, "response-referrer-policy-present=%d\n",
                   result->response_referrer_policy_header_present ? 1 : 0)
                   >= 0
        && trace_write_line(meta, "response-referrer-policy",
                            result->response_referrer_policy)
        && fprintf(meta, "response-security-headers-truncated=%d\n",
                   result->response_security_headers_truncated ? 1 : 0) >= 0
        && fprintf(
               meta,
               "response-security-metadata=%u,%u,%u,%u,%u,%u,%u,%u,%u,"
               "%llu,%u,%u,%u,%u,%u\n",
               (unsigned) captured_security.version,
               (unsigned) captured_security.allow_origin_state,
               (unsigned) captured_security.allow_credentials_state,
               captured_security.allow_credentials ? 1u : 0u,
               (unsigned) captured_security.corp_state,
               (unsigned) captured_security.corp,
               (unsigned) captured_security.nosniff_state,
               captured_security.nosniff ? 1u : 0u,
               (unsigned) captured_security.hsts_state,
               (unsigned long long) captured_security.hsts_max_age,
               captured_security.hsts_include_subdomains ? 1u : 0u,
               (unsigned) captured_security.referrer_policy_state,
               (unsigned) captured_security.csp_snapshot_state,
               (unsigned) captured_security.frame_policy_snapshot_state,
               captured_security.snapshot_truncated ? 1u : 0u) >= 0
        && trace_write_line(meta, "response-security-allow-origin",
                            captured_security.allow_origin)
        && fprintf(meta, "response-header-count=%zu\n",
                   response_header_count(result)) >= 0
        && fprintf(meta, "set-cookie-count=%zu\n",
                   result->set_cookie_count) >= 0;
    for (size_t i = 0; ok && i < result->set_cookie_count; i++) {
        char name[32];
        (void) snprintf(name, sizeof(name), "set-cookie-%zu", i);
        ok = trace_cookie_value(
                 result->set_cookies[i], scratch->header_value,
                 sizeof(scratch->header_value),
                 fetch_trace.capture_raw_cookie_values)
             && trace_write_line(meta, name, scratch->header_value);
        (void) snprintf(name, sizeof(name), "set-cookie-url-%zu", i);
        ok = ok && trace_write_line(meta, name,
                                    result->set_cookie_urls[i]);
    }
    size_t header_index = 0, header_offset = 0;
    while (ok && header_offset < result->response_headers_length) {
        const char *line = result->response_headers + header_offset;
        const char *newline = memchr(
            line, '\n', result->response_headers_length - header_offset);
        if (newline == NULL) { ok = false; break; }
        size_t line_length = (size_t) (newline - line);
        char name[40];
        if (line_length >= sizeof(scratch->header_value)) {
            ok = false;
            break;
        }
        memcpy(scratch->header_value, line, line_length);
        scratch->header_value[line_length] = '\0';
        (void) snprintf(name, sizeof(name), "response-header-%zu",
                        header_index++);
        ok = trace_write_line(meta, name, scratch->header_value);
        header_offset += line_length + 1;
    }
    if (ok && !body_previously_written && result->length != 0) {
        ok = fwrite(result->data, 1, result->length, body) == result->length;
    }
    int meta_close = fclose(meta);
    int body_close = body == NULL ? 0 : fclose(body);
    ok = meta_close == 0 && body_close == 0 && ok;
    if (!ok) {
        (void) remove(scratch->meta_path);
        (void) remove(scratch->body_path);
    }
    budget_free(budget, scratch);
    return ok;
}

static bool trace_capture_response(const char *method, const char *url,
                                   const char *logical_url,
                                   const FetchRequest *request,
                                   const char *effective_user_agent,
                                   bool diagnostic_mobile_safari,
                                   const FetchResult *result, bool success)
{
    if (fetch_trace.sequence >= FETCH_TRACE_RECORD_LIMIT) return false;
    size_t sequence = fetch_trace.sequence++;
    return trace_capture_response_at(sequence, method, url, logical_url,
                                     request,
                                     effective_user_agent,
                                     diagnostic_mobile_safari, result,
                                     success, 0, false, false);
}

static bool trace_copy_value(char *output, size_t capacity,
                             const char *value)
{
    size_t length = strlen(value);
    if (length >= capacity) return false;
    memcpy(output, value, length + 1);
    return true;
}

/* These fields describe the browser's source context, not bytes sent on the
   wire. Query strings in them can contain run-local navigation tokens. Their
   security contribution is the source origin; the actual request URL,
   Referer, Origin, Fetch Metadata, credentials policy, validators, body and
   client-hint scope remain exact replay matches below. */
static bool trace_source_context_match(const char *recorded,
                                       const char *current)
{
    const char *left = recorded == NULL ? "" : recorded;
    const char *right = current == NULL ? "" : current;
    if (strcmp(left, right) == 0) return true;
    return left[0] != '\0' && right[0] != '\0'
        && tilefinch_url_same_origin(left, right);
}

static bool fetch_urls_have_same_request_key(const char *left,
                                             const char *right)
{
    char left_key[TILEFINCH_URL_SERIALIZED_LIMIT];
    char right_key[TILEFINCH_URL_SERIALIZED_LIMIT];
    return tilefinch_url_request_key(left, left_key, sizeof(left_key))
        && tilefinch_url_request_key(right, right_key, sizeof(right_key))
        && strcmp(left_key, right_key) == 0;
}

static bool cors_response_allows_with_cached_304(
    const FetchResult *result, const char *request_origin,
    bool credentials_included, bool cached_response_validated);

static bool replay_cors_response_allows(
    const FetchRequest *request, const char *initial_url,
    FetchResult *result)
{
    if (request == NULL || !request->enforce_cors) return true;
    const char *initiator = request->initiator_url == NULL
        ? initial_url : request->initiator_url;
    const char *effective = result->effective_url[0] == '\0'
        ? initial_url : result->effective_url;
    bool required = result->redirect_origin_tainted
        || !tilefinch_url_same_origin(initiator, effective);
    if (!required) return true;
    const char *origin = result->redirect_origin_tainted
        ? "null" : request->origin;
    bool credentials_included =
        request->credentials == FETCH_CREDENTIALS_INCLUDE;
    if (request->credentials == FETCH_CREDENTIALS_SAME_ORIGIN) {
        credentials_included = !result->redirect_origin_tainted
            && tilefinch_url_same_origin(
                   request->credential_origin, initial_url)
            && tilefinch_url_same_origin(
                   request->credential_origin, effective);
    }
    bool cached_response_validated =
        request->cors_cached_response_validated
        && result->status_code == 304
        && !result->redirect_origin_tainted
        && fetch_urls_have_same_request_key(effective, initial_url);
    if (cors_response_allows_with_cached_304(
            result, origin, credentials_included,
            cached_response_validated)) return true;
    snprintf(result->error, sizeof(result->error),
             "CORS response denied by access-control policy");
    return false;
}

static void fetch_result_discard_cookies_from(FetchResult *result,
                                              size_t first)
{
    if (result == NULL) return;
    while (result->set_cookie_count > first) {
        size_t index = --result->set_cookie_count;
        budget_free(result->budget, result->set_cookies[index]);
        result->set_cookies[index] = NULL;
        budget_free(result->budget, result->set_cookie_urls[index]);
        result->set_cookie_urls[index] = NULL;
    }
}

/* Replay can encounter an out-of-order or malformed cookie index before it
   knows the record is unusable.  Clear every slot, not just the contiguous
   prefix represented by set_cookie_count, so failure remains transactional. */
static void fetch_result_discard_all_cookie_storage(FetchResult *result)
{
    if (result == NULL) return;
    for (size_t i = 0; i < FETCH_RESPONSE_COOKIE_CAPACITY; i++) {
        budget_free(result->budget, result->set_cookies[i]);
        result->set_cookies[i] = NULL;
        budget_free(result->budget, result->set_cookie_urls[i]);
        result->set_cookie_urls[i] = NULL;
    }
    result->set_cookie_count = 0;
}

static bool trace_parse_size_value(const char *value, size_t *parsed)
{
    if (value == NULL || value[0] == '\0' || parsed == NULL) return false;
    size_t result = 0;
    for (const unsigned char *at = (const unsigned char *) value;
         *at != '\0'; at++) {
        if (*at < '0' || *at > '9') return false;
        size_t digit = (size_t) (*at - '0');
        if (result > (SIZE_MAX - digit) / 10u) return false;
        result = result * 10u + digit;
    }
    *parsed = result;
    return true;
}

static bool trace_parse_hash_value(const char *value, uint64_t *parsed)
{
    if (value == NULL || value[0] == '\0' || parsed == NULL) return false;
    uint64_t result = 0;
    size_t digits = 0;
    for (const unsigned char *at = (const unsigned char *) value;
         *at != '\0'; at++, digits++) {
        unsigned digit;
        if (*at >= '0' && *at <= '9') digit = (unsigned) (*at - '0');
        else if (*at >= 'a' && *at <= 'f')
            digit = 10u + (unsigned) (*at - 'a');
        else if (*at >= 'A' && *at <= 'F')
            digit = 10u + (unsigned) (*at - 'A');
        else return false;
        if (digits >= 16) return false;
        result = (result << 4) | digit;
    }
    if (digits != 16) return false;
    *parsed = result;
    return true;
}

static bool trace_parse_bool_value(const char *value, int *parsed)
{
    if (value == NULL || parsed == NULL
        || (strcmp(value, "0") != 0 && strcmp(value, "1") != 0)) {
        return false;
    }
    *parsed = value[0] == '1';
    return true;
}

typedef enum {
    TRACE_V6_VERSION,
    TRACE_V6_COOKIE_VALUES,
    TRACE_V6_METHOD,
    TRACE_V6_URL,
    TRACE_V6_SUCCESS,
    TRACE_V6_ASYNC_DELAY,
    TRACE_V6_EXTERNAL_CANCEL,
    TRACE_V6_TRANSPORT_TIMEOUT,
    TRACE_V6_ERROR,
    TRACE_V6_REQUEST_BODY_LENGTH,
    TRACE_V6_REQUEST_BODY_HASH,
    TRACE_V6_REQUEST_CONTENT_TYPE,
    TRACE_V6_REQUEST_COOKIE_BYTES,
    TRACE_V6_REQUEST_CF_CLEARANCE,
    TRACE_V6_REQUEST_EXTRA_BYTES,
    TRACE_V6_REQUEST_EXTRA_SHAPE,
    TRACE_V6_REQUEST_ALLOW_ERRORS,
    TRACE_V6_REQUEST_IF_NONE_MATCH,
    TRACE_V6_REQUEST_IF_MODIFIED_SINCE,
    TRACE_V6_REQUEST_REFERER,
    TRACE_V6_REQUEST_ORIGIN,
    TRACE_V6_REQUEST_ACCEPT,
    TRACE_V6_REQUEST_FETCH_DEST,
    TRACE_V6_REQUEST_FETCH_MODE,
    TRACE_V6_REQUEST_FETCH_SITE,
    TRACE_V6_REQUEST_CLIENT_HINTS,
    TRACE_V6_REQUEST_LOW_CLIENT_HINTS,
    TRACE_V6_REQUEST_FETCH_USER,
    TRACE_V6_REQUEST_UPGRADE_INSECURE,
    TRACE_V6_REQUEST_USER_AGENT,
    TRACE_V6_REQUEST_DIAGNOSTIC_SAFARI,
    TRACE_V6_REQUEST_CREDENTIALS,
    TRACE_V6_REQUEST_CREDENTIAL_ORIGIN,
    TRACE_V6_REQUEST_INITIATOR_URL,
    TRACE_V6_REQUEST_REFERRER_SOURCE,
    TRACE_V6_REQUEST_REFERRER_POLICY,
    TRACE_V6_STATUS,
    TRACE_V6_LENGTH,
    TRACE_V6_RESPONSE_BODY_HASH,
    TRACE_V6_EFFECTIVE_URL,
    TRACE_V6_CONTENT_TYPE,
    TRACE_V6_ETAG,
    TRACE_V6_LAST_MODIFIED,
    TRACE_V6_CF_MITIGATED,
    TRACE_V6_ACCEPT_CH,
    TRACE_V6_CRITICAL_CH,
    TRACE_V6_SERVER,
    TRACE_V6_CF_RAY,
    TRACE_V6_RESPONSE_HEADER_COUNT,
    TRACE_V6_SET_COOKIE_COUNT,
    TRACE_V6_FIELD_COUNT,
    TRACE_V7_REQUEST_CLIENT_HINT_TOKENS = TRACE_V6_FIELD_COUNT,
    TRACE_V7_REQUEST_CLIENT_HINT_ORIGIN,
    TRACE_V7_FIELD_COUNT,
    TRACE_V8_REDIRECT_ORIGIN_TAINTED = TRACE_V7_FIELD_COUNT,
    TRACE_V8_REQUEST_ENFORCE_CORS,
    TRACE_V8_REQUEST_REDIRECT_SAME_ORIGIN_ONLY,
    TRACE_V8_REQUEST_CORS_CACHED_RESPONSE_VALIDATED,
    TRACE_V8_FIELD_COUNT,
    TRACE_V9_LOGICAL_REQUEST_URL = TRACE_V8_FIELD_COUNT,
    TRACE_V9_RESPONSE_REFERRER_POLICY_METADATA_VALID,
    TRACE_V9_RESPONSE_REFERRER_POLICY_PRESENT,
    TRACE_V9_RESPONSE_REFERRER_POLICY,
    TRACE_V9_FIELD_COUNT,
    TRACE_V10_RESPONSE_SECURITY_HEADERS_TRUNCATED = TRACE_V9_FIELD_COUNT,
    TRACE_V10_FIELD_COUNT,
    TRACE_V11_RESPONSE_SECURITY_METADATA = TRACE_V10_FIELD_COUNT,
    TRACE_V11_RESPONSE_SECURITY_ALLOW_ORIGIN,
    TRACE_V11_FIELD_COUNT
} TraceV6Field;

_Static_assert(TRACE_V11_FIELD_COUNT <= 64,
               "HTTP trace field mask must fit in uint64_t");

static bool trace_mark_field(uint64_t *seen, TraceV6Field field)
{
    uint64_t bit = UINT64_C(1) << (unsigned) field;
    if ((*seen & bit) != 0) return false;
    *seen |= bit;
    return true;
}

static uint64_t trace_required_field_mask(unsigned count)
{
    return count >= 64 ? UINT64_MAX
                       : (UINT64_C(1) << count) - UINT64_C(1);
}

typedef struct {
    char meta_path[4096];
    char body_path[4096];
    char line[FETCH_EXTRA_HEADERS_MAX + 256];
    char logical_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    char request_key[TILEFINCH_URL_SERIALIZED_LIMIT];
    char request_initiator_url[TILEFINCH_URL_SERIALIZED_LIMIT];
    char request_referrer_source[TILEFINCH_URL_SERIALIZED_LIMIT];
    char request_referrer_policy[64];
    char request_client_hint_tokens[1024];
    char request_client_hint_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    char request_extra_header_shape[FETCH_EXTRA_HEADERS_MAX + 1];
} TraceReplayScratch;

typedef enum {
    TRACE_REPLAY_RECORD_NO_MATCH = 0,
    TRACE_REPLAY_RECORD_MATCHED,
    TRACE_REPLAY_RECORD_INVALID
} TraceReplayRecordStatus;
